// Exercise the real worker with mocked SDK APIs, not real codec/VO hardware.
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
static int memory_fd;
static int test_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/mem")) return dup(memory_fd);
    return ::open(path, flags);
}
#define open test_open
#include "../little/src/playback/lawrec_playback.cpp"
#undef open

static int mode, reads, pools, leases, displays, channels;
static unsigned decoded_frames;
static bool eos_seen;
static int audio_channels, audio_outputs, audio_packets;
int lawrec_media_acquire(int) { ++leases; return 0; }
void lawrec_media_release(int) { --leases; }
extern "C" int lawrec_playback_display_request(int on) { displays = on; return 0; }
extern "C" int kd_mp4_create(KD_HANDLE *h, k_mp4_config_s *) { *h = (void *)1; reads = 0; return 0; }
extern "C" int kd_mp4_destroy(KD_HANDLE) { return 0; }
extern "C" int kd_mp4_get_file_info(KD_HANDLE, k_mp4_file_info_s *i) { i->track_num = mode >= 5 ? 2 : 1; i->duration = 1000; return 0; }
extern "C" int kd_mp4_get_track_by_index(KD_HANDLE, uint32_t index, k_mp4_track_info_s *t)
{
    if (index == 1) {
        t->track_type = K_MP4_STREAM_AUDIO;
        t->audio_info.codec_id = K_MP4_CODEC_ID_G711A;
        t->audio_info.channels = 1; t->audio_info.sample_rate = 8000;
        return 0;
    }
    t->track_type = K_MP4_STREAM_VIDEO;
    t->video_info.codec_id = mode == 1 ? K_MP4_CODEC_ID_H265 : K_MP4_CODEC_ID_H264;
    t->video_info.width = 1280; t->video_info.height = 720;
    return 0;
}
extern "C" int kd_mp4_get_frame(KD_HANDLE, k_mp4_frame_data_s *f)
{
    static uint8_t bytes[] = {0,0,0,1,0x65,0x01};
    if (reads >= (mode == 3 ? 1000 : mode >= 5 ? 4 : 2)) { f->eof = 1; return 0; }
    f->data = bytes; f->data_length = sizeof(bytes); f->codec_id = K_MP4_CODEC_ID_H264;
    f->time_stamp = reads++ * 33;
    if (mode >= 5 && reads % 2 == 0) f->codec_id = K_MP4_CODEC_ID_G711A;
    return 0;
}
extern "C" k_s32 kd_mapi_vb_create_pool(k_vb_pool_config *) { ++pools; return pools; }
extern "C" k_s32 kd_mapi_vb_destory_pool(k_u32) { --pools; return 0; }
extern "C" k_s32 kd_mapi_sys_get_vb_block_from_pool_id(k_u32, k_u64 *p, k_u64, const char *) { *p = 4096; return 0; }
extern "C" k_s32 kd_mapi_sys_release_vb_block(k_u64, k_u64) { return 0; }
extern "C" k_s32 kd_mapi_vdec_init(k_u32, const k_vdec_chn_attr *) { ++channels; decoded_frames = 0; eos_seen = false; return 0; }
extern "C" k_s32 kd_mapi_vdec_deinit(k_u32) { --channels; return 0; }
extern "C" k_s32 kd_mapi_vdec_start(k_u32) { return 0; }
extern "C" k_s32 kd_mapi_vdec_stop(k_u32) { return 0; }
extern "C" k_s32 kd_mapi_vdec_bind_vo(k_u32, k_u32, k_u32) { return mode == 2 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_vdec_unbind_vo(k_u32, k_u32, k_u32) { return mode == 4 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_vdec_send_stream(k_u32, k_vdec_stream *s, k_s32)
{ if (s->end_of_stream) eos_seen = true; else ++decoded_frames; return 0; }
extern "C" k_s32 kd_mapi_vdec_query_status(k_u32, k_vdec_chn_status *s)
{ s->dec_stream_frames = decoded_frames; s->end_of_stream = eos_seen ? K_TRUE : K_FALSE; return 0; }
extern "C" k_s32 kd_mapi_ao_init(k_u32, k_u32, const k_aio_dev_attr *a, k_handle *h) {
    assert(a->kd_audio_attr.i2s_attr.sample_rate == 8000);
    ++audio_outputs; *h = 0; return 0;
}
extern "C" k_s32 kd_mapi_ao_start(k_handle) { return 0; }
extern "C" k_s32 kd_mapi_ao_stop(k_handle) { return 0; }
extern "C" k_s32 kd_mapi_ao_deinit(k_handle) { --audio_outputs; return 0; }
extern "C" k_s32 kd_mapi_adec_init(k_handle, const k_adec_chn_attr *a) {
    assert(a->type == K_PT_G711A); ++audio_channels; return 0;
}
extern "C" k_s32 kd_mapi_adec_start(k_handle) { return 0; }
extern "C" k_s32 kd_mapi_adec_stop(k_handle) { return 0; }
extern "C" k_s32 kd_mapi_adec_deinit(k_handle) { --audio_channels; return 0; }
extern "C" k_s32 kd_mapi_adec_bind_ao(k_handle, k_handle) { return mode == 6 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_adec_unbind_ao(k_handle, k_handle) { return 0; }
extern "C" k_s32 kd_mapi_adec_send_stream(k_handle, const k_audio_stream *s) {
    assert(s->phys_addr == 4096 && s->stream && s->len == 6);
    assert(s->time_stamp == 33000 || s->time_stamp == 99000);
    ++audio_packets; return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(setenv("LAWREC_RECORD_DIR", argv[1], 1) == 0);
    assert(mkdir(argv[1], 0700) == 0);
    std::string path = std::string(argv[1]) + "/test.mp4";
    FILE *fp = fopen(path.c_str(), "wb"); assert(fp); fputs("fixture", fp); fclose(fp);
    char backing[] = "/tmp/lawrec-playback-test-XXXXXX";
    memory_fd = mkstemp(backing); assert(memory_fd >= 0); unlink(backing);
    assert(ftruncate(memory_fd, kStreamBytes + 4096) == 0);
    for (int scenario : {0, 1, 2, 3, 5, 6, 4}) {
        mode = scenario;
        assert(lawrec_playback_start("test.mp4") == 0);
        if (mode == 3) {
            auto limit = Clock::now() + std::chrono::seconds(2);
            lawrec_playback_status s{};
            do { lawrec_playback_get_status(&s); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            while (s.state != LAWREC_PLAY_PLAYING && Clock::now() < limit);
            assert(s.state == LAWREC_PLAY_PLAYING);
            assert(lawrec_playback_start("test.mp4") == -EBUSY);
            lawrec_playback_pause(1);
            lawrec_playback_get_status(&s); assert(s.state == LAWREC_PLAY_PAUSED);
            assert(lawrec_playback_stop_wait(2000) == 0);
        }
        auto limit = Clock::now() + std::chrono::seconds(3);
        while (active && Clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!active); worker.join();
        lawrec_playback_status s{}; lawrec_playback_get_status(&s);
        assert(s.state == (mode == 0 || mode == 5 ? LAWREC_PLAY_FINISHED : mode == 3 ? LAWREC_PLAY_IDLE : LAWREC_PLAY_FAILED));
        assert(audio_channels == 0 && audio_outputs == 0);
        if (mode == 5) assert(audio_packets == 2);
        if (mode == 4) {
            assert(pools == 2 && leases == 1 && lawrec_playback_active());
            assert(lawrec_playback_start("test.mp4") == -EBUSY);
        } else assert(pools == 0 && leases == 0 && displays == 0 && channels == 0);
    }
    close(memory_fd); unlink(path.c_str()); rmdir(argv[1]);
    puts("Playback mock: EOF, format rejection, bind failure, pause/stop, teardown quarantine passed");
}
