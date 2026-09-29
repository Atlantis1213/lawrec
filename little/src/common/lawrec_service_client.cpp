#include "lawrec_service_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int recv_all(int fd, void *buf, size_t size)
{
    size_t got = 0;
    char *p = static_cast<char *>(buf);

    while (got < size) {
        ssize_t ret = recv(fd, p + got, size - got, 0);
        if (ret <= 0)
            return -EIO;
        got += static_cast<size_t>(ret);
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t size)
{
    size_t sent = 0;
    const char *p = static_cast<const char *>(buf);

    while (sent < size) {
        ssize_t ret = send(fd, p + sent, size - sent, 0);
        if (ret <= 0)
            return -EIO;
        sent += static_cast<size_t>(ret);
    }
    return 0;
}

extern "C" int lawrec_service_call(lawrec_service_cmd_e cmd, uint32_t value,
                                    lawrec_service_response_t *resp)
{
    int fd;
    int ret;
    struct sockaddr_un addr;
    lawrec_service_request_t req;

    if (resp == nullptr)
        return -EINVAL;

    memset(resp, 0, sizeof(*resp));
    resp->magic = LAWREC_SERVICE_MSG_MAGIC;
    resp->version = LAWREC_SERVICE_MSG_VERSION;
    resp->result = -1;
    resp->error_no = ENOTCONN;
    snprintf(resp->message, sizeof(resp->message), "%s", "service unavailable");

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        ret = -errno;
        resp->error_no = errno;
        snprintf(resp->message, sizeof(resp->message),
                 "socket failed:%d", errno);
        return ret;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
             LAWREC_SERVICE_SOCKET_PATH);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr),
                sizeof(addr)) != 0) {
        ret = -errno;
        resp->error_no = errno;
        snprintf(resp->message, sizeof(resp->message),
                 "connect failed:%d", errno);
        close(fd);
        return ret;
    }

    memset(&req, 0, sizeof(req));
    req.magic = LAWREC_SERVICE_MSG_MAGIC;
    req.version = LAWREC_SERVICE_MSG_VERSION;
    req.cmd = static_cast<uint32_t>(cmd);
    req.value = value;

    ret = send_all(fd, &req, sizeof(req));
    if (ret == 0)
        ret = recv_all(fd, resp, sizeof(*resp));
    close(fd);

    if (ret != 0) {
        resp->result = ret;
        resp->error_no = -ret;
        snprintf(resp->message, sizeof(resp->message),
                 "service io failed:%d", ret);
        return ret;
    }

    if (resp->magic != LAWREC_SERVICE_MSG_MAGIC ||
        resp->version != LAWREC_SERVICE_MSG_VERSION) {
        resp->result = -EPROTO;
        resp->error_no = EPROTO;
        snprintf(resp->message, sizeof(resp->message), "%s",
                 "bad service response");
        return -EPROTO;
    }

    return resp->result;
}
