// Parser lifecycle tests. Native mode links the actual, assertion-enabled SDK.
#include "../little/src/playback/lawrec_demux.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static std::string directory;

static void write_file(const std::string &path, const std::vector<unsigned char> &bytes)
{
    FILE *file = fopen(path.c_str(), "wb"); assert(file);
    assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(fclose(file) == 0);
}

static void no_children()
{
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

#ifdef LAWREC_DEMUX_MOCK
static int mode, frames;
static void hang()
{
    write_file(std::string(getenv("LAWREC_DEMUX_MARKER")), {1});
    for (;;) pause();
}
extern "C" int kd_mp4_create(KD_HANDLE *handle, k_mp4_config_s *)
{
    // This intentionally non-CLOEXEC parent descriptor must be removed before
    // any SDK parsing. The parent still owns its original copy.
    assert(fcntl(100, F_GETFD) == -1 && errno == EBADF);
    if (mode == 10) abort();
    if (mode == 11) hang();
    *handle = reinterpret_cast<void *>(1); return 0;
}
extern "C" int kd_mp4_destroy(KD_HANDLE)
{
    if (mode == 20) abort();
    if (mode == 21) hang();
    return mode == 22 ? -1 : 0;
}
extern "C" int kd_mp4_get_file_info(KD_HANDLE, k_mp4_file_info_s *info)
{ info->track_num = mode == 23 ? 3 : 1; info->duration = 33; return 0; }
extern "C" int kd_mp4_get_track_by_index(KD_HANDLE, uint32_t, k_mp4_track_info_s *track)
{
    track->track_type = K_MP4_STREAM_VIDEO;
    track->video_info = {1280, 720, 1, K_MP4_CODEC_ID_H264}; return 0;
}
extern "C" int kd_mp4_get_frame(KD_HANDLE, k_mp4_frame_data_s *frame)
{
    if (mode == 12) abort();
    if (mode == 13) hang();
    static unsigned char bytes[] = {0, 0, 0, 1, 0x65, 0x80};
    if (frames++) { frame->eof = 1; return 0; }
    frame->data = mode == 15 ? nullptr : bytes;
    frame->data_length = mode == 14 ? 4*1024*1024+1 : sizeof(bytes);
    frame->codec_id = mode == 16 ? K_MP4_CODEC_ID_BUTT : K_MP4_CODEC_ID_H264;
    return 0;
}

static void select(int value)
{
    std::string text = std::to_string(value);
    assert(setenv("LAWREC_DEMUX_MODE", text.c_str(), 1) == 0);
    unlink((directory+"/entered").c_str());
}

static void cancellation(int file, bool at_start)
{
    select(at_start ? 11 : 13);
    std::atomic<bool> cancel{false};
    LawrecDemux demux(cancel);
    k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
    if (!at_start) assert(demux.start(file, info, tracks) == 0);
    Clock::time_point signalled;
    std::thread canceller([&] {
        auto deadline = Clock::now()+std::chrono::seconds(2);
        while (access((directory+"/entered").c_str(), F_OK) && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(access((directory+"/entered").c_str(), F_OK) == 0);
        signalled = Clock::now(); cancel = true;
    });
    k_mp4_frame_data_s frame{};
    int result = at_start ? demux.start(file, info, tracks) : demux.next(frame);
    canceller.join();
    assert(result == -ECANCELED);
    assert(demux.close() == -ETIMEDOUT);
    assert(Clock::now()-signalled < std::chrono::seconds(2));
    no_children();
}
#else
static void make_fixture(const std::string &path)
{
    k_mp4_config_s config{}; config.config_type = K_MP4_CONFIG_MUXER;
    assert(path.size() < sizeof(config.muxer_config.file_name));
    strcpy(config.muxer_config.file_name, path.c_str());
    KD_HANDLE handle, track;
    assert(kd_mp4_create(&handle, &config) == 0);
    k_mp4_track_info_s info{};
    info.track_type = K_MP4_STREAM_VIDEO; info.time_scale = 1000;
    info.video_info = {1280, 720, 1, K_MP4_CODEC_ID_H264};
    assert(kd_mp4_create_track(handle, &track, &info) == 0);
    // Structurally valid container NALs, not a claim of decodable video pixels.
    unsigned char bytes[] = {0,0,0,1,0x67,0x42,0xe0,0x1e,0xab,0xcd,
                             0,0,0,1,0x68,0xce,0x3c,0x80,
                             0,0,0,1,0x65,0x80,0x01};
    k_mp4_frame_data_s frame{};
    frame.codec_id = K_MP4_CODEC_ID_H264; frame.data = bytes; frame.data_length = sizeof(bytes);
    assert(kd_mp4_write_frame(handle, track, &frame) == 0);
    frame.time_stamp = 33000;
    assert(kd_mp4_write_frame(handle, track, &frame) == 0);
    assert(kd_mp4_destroy_tracks(handle) == 0);
    assert(kd_mp4_destroy(handle) == 0);
}

static void damaged(const std::string &name, const std::vector<unsigned char> &bytes, bool frame_failure = false)
{
    std::string path = directory+"/"+name;
    write_file(path, bytes);
    int file = open(path.c_str(), O_RDONLY | O_CLOEXEC); assert(file >= 0);
    std::atomic<bool> cancel{false};
    LawrecDemux demux(cancel);
    k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
    int result = demux.start(file, info, tracks);
    if (frame_failure) assert(result == 0);
    if (!result) {
        k_mp4_frame_data_s frame{};
        result = demux.next(frame);
    }
    assert(result < 0);
    demux.close(); close(file); unlink(path.c_str()); no_children();
    printf("Native SDK corruption: %s result=%d\n", name.c_str(), result);
}
#endif

static void valid(int file)
{
    std::atomic<bool> cancel{false};
    LawrecDemux demux(cancel);
    k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
    assert(demux.start(file, info, tracks) == 0);
    assert(info.track_num == 1 && tracks[0].video_info.width == 1280);
    LawrecDemux other(cancel);
    assert(other.start(file, info, tracks) == -EBUSY);
    k_mp4_frame_data_s frame{};
    int count = 0;
    do {
        assert(demux.next(frame) == 0);
        if (!frame.eof) { assert(frame.data && frame.data_length); ++count; }
    } while (!frame.eof && count < 10);
    assert(frame.eof && count > 0);
    assert(demux.close() == 0);
    assert(demux.close() == 0); no_children();
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--mp4-demux")) {
#ifdef LAWREC_DEMUX_MOCK
        mode = atoi(getenv("LAWREC_DEMUX_MODE"));
#endif
        return lawrec_demux_helper(argc, argv);
    }
    char temp[] = "out/tests/demux-XXXXXX";
    assert(mkdtemp(temp)); directory = temp;
    std::string path = directory+"/valid.mp4";
#ifdef LAWREC_DEMUX_MOCK
    write_file(path, {1});
    assert(setenv("LAWREC_DEMUX_MARKER", (directory+"/entered").c_str(), 1) == 0);
    select(0);
#else
    make_fixture(path);
#endif
    int file = open(path.c_str(), O_RDONLY | O_CLOEXEC); assert(file >= 0);
    assert(dup2(file, 100) == 100);
    valid(file);
    assert(fcntl(100, F_GETFD) >= 0);
    {
        int writable = open(path.c_str(), O_RDWR | O_CLOEXEC); assert(writable >= 0);
        std::atomic<bool> cancel{false}; LawrecDemux demux(cancel);
        k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
        assert(demux.start(writable, info, tracks) < 0);
        assert(demux.close() == 0);
        close(writable); no_children(); valid(file);
        int closed = dup(file); assert(closed >= 0); close(closed);
        assert(demux.start(closed, info, tracks) == -EBADF);
        valid(file);
    }
#ifdef LAWREC_DEMUX_MOCK
    for (int scenario : {10, 12, 14, 15, 16, 20, 21, 22, 23}) {
        select(scenario);
        std::atomic<bool> cancel{false}; LawrecDemux demux(cancel);
        k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
        int result = demux.start(file, info, tracks);
        if (scenario == 10 || scenario == 23) assert(result < 0);
        else {
            assert(result == 0);
            k_mp4_frame_data_s frame{};
            result = demux.next(frame);
            assert((scenario < 20) == (result < 0));
        }
        int closed = demux.close();
        if (scenario >= 20 && scenario <= 22) assert(closed < 0);
        no_children(); select(0); valid(file);
    }
    cancellation(file, true); cancellation(file, false);
    select(13);
    std::atomic<bool> cancel{false}; LawrecDemux demux(cancel);
    k_mp4_file_info_s info{}; k_mp4_track_info_s tracks[2]{};
    assert(demux.start(file, info, tracks) == 0);
    k_mp4_frame_data_s frame{}; auto before = Clock::now();
    assert(demux.next(frame) == -ETIMEDOUT);
    assert(Clock::now()-before >= std::chrono::seconds(8));
    assert(Clock::now()-before < std::chrono::seconds(10));
    assert(demux.close() == -ETIMEDOUT); no_children();
    select(0); valid(file);
    unlink((directory+"/entered").c_str());
    puts("Demux mock: create/frame/destroy crashes, invalid frames, cancellation, deadline, restart passed");
#else
    // A malformed ftyp payload provokes the SDK's real mov_read_ftyp assert.
    damaged("bad-ftyp.mp4", {0,0,0,17,'f','t','y','p','i','s','o','m',0,0,0,0,1});
    damaged("no-moov.mp4", {0,0,0,16,'f','t','y','p','i','s','o','m',0,0,0,0});
    damaged("truncated-box.mp4", {0,0,0,32,'m','o','o','v',0});
    FILE *input = fopen(path.c_str(), "rb"); assert(input);
    assert(fseek(input, 0, SEEK_END) == 0); long size = ftell(input); assert(size > 0);
    rewind(input); std::vector<unsigned char> bytes(size);
    assert(fread(bytes.data(), 1, bytes.size(), input) == bytes.size()); fclose(input);
    bool patched = false;
    for (size_t i = 4; i+10 < bytes.size(); ++i) {
        if (!memcmp(bytes.data()+i, "mdat", 4)) {
            // First sample starts with a four-byte NAL length. Replace the
            // first NAL (normally SPS) with a non-first VCL slice so the SDK
            // get_frame assertion, not a later video decoder, rejects it.
            bytes[i+8] = 0x65; bytes[i+9] = 0; patched = true;
            break;
        }
    }
    assert(patched); damaged("bad-access-unit.mp4", bytes, true);
    valid(file);
    puts("Demux native SDK: valid container, parser/frame assertions, truncated box, missing moov, restart passed");
#endif
    close(100); close(file); unlink(path.c_str()); assert(rmdir(directory.c_str()) == 0);
}
