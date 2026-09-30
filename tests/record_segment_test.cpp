// Execute the production encoder callback with mocked storage/MP4 APIs.
#include "../little/src/record/src/lawrec_record_entry.cpp"
#include <cassert>

struct Sample { uint64_t pts; std::vector<uint8_t> bytes; };
struct Segment { std::vector<Sample> frames, audio; bool closed = false, published = false; };
static std::vector<Segment> segments;
static bool fail_reserve, fail_write, fail_close;
extern "C" int lawrec_settings_segment_seconds() { return 1; }
extern "C" int lawrec_settings_audio_enabled() { return 0; }
extern "C" int lawrec_storage_reserve(char *path, size_t size) {
    if (fail_reserve) return -ENOSPC;
    snprintf(path, size, "/mock/record_%zu.part", segments.size()+1); return 0;
}
extern "C" int lawrec_storage_publish(const char *path, char *final, size_t size) {
    unsigned id = 0; assert(sscanf(path, "/mock/record_%u.part", &id) == 1);
    assert(id && id <= segments.size());
    assert(segments[id-1].closed && !segments[id-1].frames.empty());
    segments[id-1].published = true;
    snprintf(final, size, "/mock/record_%u.mp4", id); return 0;
}
extern "C" int kd_mp4_create(KD_HANDLE *handle, k_mp4_config_s *) {
    segments.emplace_back(); *handle = reinterpret_cast<KD_HANDLE>(segments.size()); return 0;
}
extern "C" int kd_mp4_create_track(KD_HANDLE, KD_HANDLE *track, k_mp4_track_info_s *info) {
    if (info->track_type == K_MP4_STREAM_VIDEO)
        assert(info->time_scale == 1000 && info->video_info.width == 1280);
    else assert(info->audio_info.codec_id == K_MP4_CODEC_ID_G711A && info->audio_info.sample_rate == 8000);
    *track = reinterpret_cast<KD_HANDLE>(1); return 0;
}
extern "C" int kd_mp4_destroy_tracks(KD_HANDLE) { return 0; }
extern "C" int kd_mp4_destroy(KD_HANDLE handle) {
    segments[reinterpret_cast<uintptr_t>(handle)-1].closed = true;
    return fail_close ? -1 : 0;
}
extern "C" int kd_mp4_write_frame(KD_HANDLE handle, KD_HANDLE, k_mp4_frame_data_s *frame) {
    if (fail_write) return -1;
    auto &segment = segments[reinterpret_cast<uintptr_t>(handle)-1];
    assert(!segment.closed);
    auto &output = frame->codec_id == K_MP4_CODEC_ID_G711A ? segment.audio : segment.frames;
    output.push_back({frame->time_stamp,
        std::vector<uint8_t>(frame->data, frame->data + frame->data_length)});
    return 0;
}

static int feed(uint64_t ms, const std::vector<uint8_t> &bytes) {
    kd_venc_data_s data{};
    data.status.cur_packs = 1;
    data.astPack[0].vir_addr = reinterpret_cast<char *>(const_cast<uint8_t *>(bytes.data()));
    data.astPack[0].len = bytes.size(); data.astPack[0].pts = ms*1000;
    return record_video_callback(1, &data, nullptr);
}
const std::vector<uint8_t> headers = {0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xce};
const std::vector<uint8_t> idr = {0,0,0,1,0x65,0x11};
const std::vector<uint8_t> delta = {0,0,0,1,0x41,0x22};
static void begin() {
    fail_reserve = fail_write = fail_close = false;
    close_segment_locked(false);
    segments.clear();
    reset_runtime_fields_locked();
    g_record.config.video_width = 1280; g_record.config.video_height = 720;
    g_record.stop_requested = false; g_record.last_error = 0;
    assert(open_segment_locked() == 0);
    assert(feed(1000, headers) == 0 && segments[0].frames.empty());
    assert(feed(1000, delta) == 0 && segments[0].frames.empty());
    assert(feed(1000, idr) == 0 && segments[0].frames.size() == 1);
}
int main() {
    begin();
    g_record.audio_enabled = true;
    g_record_audio_queue.reset();
    auto audio = std::make_shared<LawrecEncodedFrame>();
    audio->bytes.assign(320, 0xd5);
    audio->pts_us = 1040000;
    assert(g_record_audio_queue.push(audio) == 0);
    LawrecFramePtr pending;
    auto progress = std::chrono::steady_clock::now();
    assert(record_audio_before(1030000, pending, progress) == 0 && pending);
    assert(record_audio_before(1080000, pending, progress) == 0 && !pending);
    assert(segments[0].audio.size() == 1 && segments[0].audio[0].pts == 40000);
    assert(feed(2500, idr) == 0 && segments[0].published);
    audio->pts_us = 2540000;
    assert(g_record_audio_queue.push(audio) == 0);
    assert(record_audio_before(2580000, pending, progress) == 0);
    assert(segments[1].audio.size() == 1 && segments[1].audio[0].pts == 40000);
    assert(close_segment_locked(true) == 0);
    begin();
    g_record.audio_enabled = true;
    assert(close_segment_locked(true) == -ENODATA && !segments[0].published);
    begin();
    assert(feed(1900, delta) == 0);
    assert(feed(2200, delta) == 0 && segments.size() == 1);
    assert(feed(2500, idr) == 0 && segments.size() == 2);
    assert(segments[0].published && segments[0].frames.size() == 3);
    assert(segments[0].frames[2].pts == 1200000);
    assert(segments[1].frames.size() == 1 && segments[1].frames[0].pts == 0);
    auto first = headers; first.insert(first.end(), idr.begin(), idr.end());
    assert(segments[1].frames[0].bytes == first);
    assert(g_record.callback_count == 4 && g_record.got_first_frame);
    assert(close_segment_locked(true) == 0 && segments[1].published);
    begin(); fail_reserve = true;
    assert(feed(2500, idr) == -ENOSPC && g_record.stop_requested);
    assert(segments.size() == 1 && segments[0].published);
    begin(); fail_close = true;
    assert(feed(2500, idr) == -EIO && g_record.stop_requested);
    assert(segments.size() == 1 && !segments[0].published);
    begin(); fail_write = true;
    assert(feed(2500, idr) == -EIO && g_record.stop_requested);
    assert(segments[0].published && !segments[1].published);
    begin();
    assert(feed(900, delta) == -EINVAL && g_record.stop_requested);
    assert(segments[0].frames.size() == 1);
    begin(); g_record.segment_seconds = 0;
    assert(feed(2500, idr) == 0 && segments.size() == 1);
    g_record.stop_requested = true;
    assert(feed(3500, idr) == 0 && segments[0].frames.size() == 2);
    close_segment_locked(false);
    puts("record segments: video/audio PTS, missing audio, IDR boundaries, headers, storage/close/write failure, stop passed");
}
