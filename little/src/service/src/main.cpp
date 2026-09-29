#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../common/lawrec_service_protocol.h"
#include "../../control/include/lawrec_control.h"

static const char *kSocketPath = LAWREC_SERVICE_SOCKET_PATH;
static const char *kLogPath = "/tmp/lawrec-service.log";

static volatile sig_atomic_t g_stop = 0;

static void log_line(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    time_t now;
    struct tm tm_now;
    char buf[32];

    fp = fopen(kLogPath, "a");
    if (fp == NULL)
        return;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), "%F %T", &tm_now);
    fprintf(fp, "[lawrec-service] %s ", buf);

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}


static int handle_request(const lawrec_service_request_t *req,
                          lawrec_service_response_t *resp)
{
    /* socket service 只做薄封装，RTSP 策略统一由 control 模块负责。 */
    if (req->magic != LAWREC_SERVICE_MSG_MAGIC ||
        req->version != LAWREC_SERVICE_MSG_VERSION) {
        memset(resp, 0, sizeof(*resp));
        resp->magic = LAWREC_SERVICE_MSG_MAGIC;
        resp->version = LAWREC_SERVICE_MSG_VERSION;
        resp->result = -1;
        resp->error_no = EPROTO;
        snprintf(resp->message, sizeof(resp->message), "bad request");
        log_line("reject bad request magic=0x%x version=%u cmd=%u",
                 req->magic, req->version, req->cmd);
        return -1;
    }

    log_line("request cmd=%u", req->cmd);

    switch ((lawrec_service_cmd_e)req->cmd) {
    case LAWREC_SERVICE_CMD_RECORD_START:
    case LAWREC_SERVICE_CMD_RECORD_STOP:
    case LAWREC_SERVICE_CMD_RECORD_QUERY:
        return lawrec_control_handle_record_cmd(
            (lawrec_service_cmd_e)req->cmd, resp);
    case LAWREC_SERVICE_CMD_RTSP_START:
    case LAWREC_SERVICE_CMD_RTSP_STOP:
    case LAWREC_SERVICE_CMD_RTSP_QUERY:
    default:
        return lawrec_control_handle_rtsp_cmd(
            (lawrec_service_cmd_e)req->cmd, resp);
    }
}

static int recv_all(int fd, void *buf, size_t size)
{
    size_t got = 0;
    ssize_t ret;
    char *p = (char *)buf;

    while (got < size) {
        ret = recv(fd, p + got, size - got, 0);
        if (ret <= 0) {
            log_line("recv failed ret=%zd errno=%d", ret, errno);
            return -1;
        }
        got += (size_t)ret;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t size)
{
    size_t sent = 0;
    ssize_t ret;
    const char *p = (const char *)buf;

    while (sent < size) {
        ret = send(fd, p + sent, size - sent, 0);
        if (ret <= 0) {
            log_line("send failed ret=%zd errno=%d", ret, errno);
            return -1;
        }
        sent += (size_t)ret;
    }
    return 0;
}

static int run_service(void)
{
    int server_fd;
    int client_fd;
    struct sockaddr_un addr;
    lawrec_service_request_t req;
    lawrec_service_response_t resp;

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGCHLD, SIG_DFL);

    /*
     * service 和 UI 故意写入同一个日志文件，
     * 这样板端排障时可以直接还原小核侧完整时序。
     */
    lawrec_control_set_log_path(kLogPath);
    lawrec_control_init();

    unlink(kSocketPath);
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_line("socket create failed errno=%d", errno);
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kSocketPath);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_line("bind failed path=%s errno=%d", kSocketPath, errno);
        perror("bind");
        close(server_fd);
        return 1;
    }

    chmod(kSocketPath, 0666);

    if (listen(server_fd, 4) != 0) {
        log_line("listen failed path=%s errno=%d", kSocketPath, errno);
        perror("listen");
        unlink(kSocketPath);
        close(server_fd);
        return 1;
    }

    log_line("service started socket=%s", kSocketPath);

    while (!g_stop) {
        /* 标准的 Unix 域套接字请求/响应循环。 */
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            log_line("accept failed errno=%d", errno);
            usleep(100 * 1000);
            continue;
        }

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        if (recv_all(client_fd, &req, sizeof(req)) == 0)
            handle_request(&req, &resp);
        else {
            memset(&resp, 0, sizeof(resp));
            resp.magic = LAWREC_SERVICE_MSG_MAGIC;
            resp.version = LAWREC_SERVICE_MSG_VERSION;
            resp.result = -1;
            resp.error_no = EIO;
            snprintf(resp.message, sizeof(resp.message), "%s", "recv failed");
            log_line("drop client due to recv failure");
        }

        if (send_all(client_fd, &resp, sizeof(resp)) != 0)
            log_line("response send failed cmd=%u result=%d", req.cmd, resp.result);
        else
            log_line("response cmd=%u result=%d errno=%d msg=%s",
                     req.cmd, resp.result, resp.error_no, resp.message);
        close(client_fd);
    }

    /* service 退出时强制停止 RTSP，确保套接字和内部状态都被复位。 */
    lawrec_control_handle_record_cmd(LAWREC_SERVICE_CMD_RECORD_STOP, &resp);
    lawrec_control_handle_rtsp_cmd(LAWREC_SERVICE_CMD_RTSP_STOP, &resp);
    lawrec_control_deinit();
    unlink(kSocketPath);
    close(server_fd);
    log_line("service stopped");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return run_service();
}
