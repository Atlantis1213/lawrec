#include "config.h"
#include "protocol.h"
#ifndef DEMO_SOCKET_FIXTURE
#include "vision_client.h"
#include "source.h"
#include "rtsp.h"
#include "recorder.h"
#include "metrics.h"
#endif
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
}
int main(int argc, char **argv) {
    bool mock = argc == 3 && std::strcmp(argv[1], "--mock") == 0;
    if (argc != 1 && !mock) {
        std::fprintf(stderr, "Usage: media_service [--mock SOCKET]\n");
        return 2;
    }
    const char *path = mock ? argv[2] : demo::socket_path;
    sockaddr_un address{};
    if (std::strlen(path) >= sizeof(address.sun_path)) return 2;
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    char lock_path[128];
    std::snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        std::fprintf(stderr, "[media] singleton lock failed errno=%d\n", errno);
        if (lock >= 0) close(lock);
        return 1;
    }
    struct stat old{};
    if (lstat(path, &old) == 0 && (!S_ISSOCK(old.st_mode) || unlink(path) < 0)) {
        std::fprintf(stderr, "[media] refusing non-socket/stale-path failure\n");
        close(lock);
        return 1;
    }
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server < 0 || bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        std::fprintf(stderr, "[media] socket setup failed errno=%d\n", errno);
        if (server >= 0) close(server);
        close(lock);
        return 1;
    }
    chmod(path, 0600);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    demo::Status status;
    int backend_error = -ENOSYS;
#ifndef DEMO_SOCKET_FIXTURE
    demo::VisionClient vision;
    demo::MediaSource source;
    demo::RtspWorker rtsp(source);
    demo::Recorder record(source);
    demo::MediaMetrics metrics;
    if (!mock) backend_error = vision.start();
#endif
    std::fprintf(stderr, "[media] mode=%s socket=%s backend_init=%d\n", mock ? "MOCK, no hardware" : "vision IPC + shared RTSP/MP4", path, backend_error);
    while (running) {
#ifndef DEMO_SOCKET_FIXTURE
        source.tick();
        if (!mock) metrics.update(source.stats());
#endif
        pollfd listener{server, POLLIN, 0};
        if (poll(&listener, 1, 100) <= 0) continue;
        int client = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) continue;
        pollfd incoming{client, POLLIN, 0};
        if (poll(&incoming, 1, 500) > 0 && (incoming.revents & POLLIN)) {
            demo::Request request;
            ssize_t bytes = recv(client, &request, sizeof(request), MSG_TRUNC);
            if (bytes >= 0 && demo::decode_request(&request, static_cast<size_t>(bytes), request)) {
                status.id = request.id;
                status.result = mock ? 0 : backend_error;
                if (mock && request.command) {
                    uint32_t flag = 1U << (request.command - 1);
                    status.flags = request.value ? status.flags | flag : status.flags & ~flag;
                }
#ifndef DEMO_SOCKET_FIXTURE
                if (!mock) {
                    int retained_error = status.last_error;
                    if (request.command == uint32_t(demo::Command::SetRtsp) && !request.value)
                        status.result = rtsp.request(false); // STOP never requires a new peer ACK.
                    else if (request.command == uint32_t(demo::Command::SetRecord) && !request.value)
                        status.result = record.request(false);
                    else if (!backend_error) {
                        demo::Request query = request;
                        if (request.command >= uint32_t(demo::Command::SetRtsp)) { query.command = 0; query.value = 0; }
                        demo::Status answer;
                        int ret = vision.exchange(query, answer);
                        if (ret) status.result = ret;
                        else {
                            status = answer;
                            if (!status.result && request.command == uint32_t(demo::Command::SetRtsp))
                                status.result = rtsp.request(true);
                            if (!status.result && request.command == uint32_t(demo::Command::SetRecord))
                                status.result = record.request(true);
                        }
                    }
                    auto stream = rtsp.status();
                    auto recording = record.status();
                    status.flags &= demo::Preview | demo::Ai;
                    if (stream.state == demo::StreamState::Running || stream.state == demo::StreamState::Stopping)
                        status.flags |= demo::Rtsp;
                    if (recording.state == demo::StreamState::Running || recording.state == demo::StreamState::Stopping)
                        status.flags |= demo::Record;
                    status.busy &= demo::Preview | demo::Ai;
                    if (stream.state == demo::StreamState::Starting || stream.state == demo::StreamState::Stopping)
                        status.busy |= demo::Rtsp;
                    if (recording.state == demo::StreamState::Starting || recording.state == demo::StreamState::Stopping)
                        status.busy |= demo::Record;
                    status.video_queue = stream.video.depth + recording.video.depth;
                    status.audio_queue = stream.audio.depth + recording.audio.depth;
                    if (stream.error) status.last_error = stream.error;
                    if (recording.error) status.last_error = recording.error;
                    if (!status.last_error) status.last_error = retained_error;
                    metrics.apply(status);
                }
#endif
                // Preserve asynchronous vision errors across successful polling.
                if (status.result) status.last_error = status.result;
                ssize_t sent = send(client, &status, sizeof(status), MSG_NOSIGNAL);
                if (sent != sizeof(status)) std::fprintf(stderr, "[media] reply failed errno=%d\n", errno);
            }
        }
        close(client);
    }
    close(server); unlink(path); close(lock);
#ifndef DEMO_SOCKET_FIXTURE
    int stream_cleanup = rtsp.shutdown();
    int record_cleanup = record.shutdown();
    int media_cleanup = source.shutdown();
    if (!media_cleanup) media_cleanup = stream_cleanup;
    if (!media_cleanup) media_cleanup = record_cleanup;
    int cleanup = vision.stop();
    if (cleanup) std::fprintf(stderr, "[media] IPC shutdown failed=%d\n", cleanup);
    if (media_cleanup) {
        std::fprintf(stderr, "[media] SDK cleanup failed=%d; do not destroy callback owner\n", media_cleanup);
        std::fflush(nullptr); _Exit(1);
    }
    return cleanup ? 1 : 0;
#else
    return 0;
#endif
}
