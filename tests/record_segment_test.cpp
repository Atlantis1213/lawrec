// Execute the same worker-owned frame path used by production recording.
#include "../little/src/record/src/lawrec_record_entry.cpp"
#include <cassert>

struct Sample { uint64_t pts; std::vector<uint8_t> bytes; };
struct Segment {
    std::vector<Sample> frames, audio;
    uint64_t video_end = 0, audio_end = 0;
    bool closed = false, published = false;
};
static std::vector<Segment> segments;
static bool fail_reserve;
static int fail_write, fail_close, fail_encoder_stop, fail_audio_stop, fail_end;
static bool kept_audio_tail;
static RecordAudioCursor test_pending;
static auto test_progress = std::chrono::steady_clock::now();
int lawrec_encoder_unsubscribe(int) { return fail_encoder_stop; }
int lawrec_audio_unsubscribe(int, bool keep_tail) {
    kept_audio_tail = keep_tail;
    if (keep_tail) g_record_audio_queue.finish();
    else g_record_audio_queue.close();
    return fail_audio_stop;
}
void lawrec_media_release(int) {}
extern "C" int lawrec_settings_segment_seconds() { return 1; }
extern "C" int lawrec_settings_audio_enabled() { return 0; }
extern "C" int lawrec_settings_frame_rate() { return 30; }
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
    *track = reinterpret_cast<KD_HANDLE>(info->track_type == K_MP4_STREAM_VIDEO ? 1 : 2); return 0;
}
extern "C" int kd_mp4_destroy_tracks(KD_HANDLE) { return 0; }
extern "C" int kd_mp4_destroy(KD_HANDLE handle) {
    segments[reinterpret_cast<uintptr_t>(handle)-1].closed = true;
    return fail_close;
}
extern "C" int lawrec_mp4_muxer_flush(KD_HANDLE) { return 0; }
extern "C" int lawrec_mp4_muxer_stats(KD_HANDLE handle, lawrec_mp4_muxer_stats_t *stats) {
    const auto &segment = segments[reinterpret_cast<uintptr_t>(handle)-1];
    assert(!segment.closed);
    *stats = {};
    stats->sample_count = segment.frames.size() + segment.audio.size();
    stats->track_count = segment.audio.empty() ? 1 : 2;
    return 0;
}
extern "C" int lawrec_mp4_muxer_end_track(KD_HANDLE handle, KD_HANDLE track, uint64_t end) {
    if (fail_end) return fail_end;
    auto &segment = segments[reinterpret_cast<uintptr_t>(handle)-1];
    assert(!segment.closed);
    if (reinterpret_cast<uintptr_t>(track) == 1) segment.video_end = end;
    else segment.audio_end = end;
    return 0;
}
extern "C" int kd_mp4_write_frame(KD_HANDLE handle, KD_HANDLE, k_mp4_frame_data_s *frame) {
    if (fail_write) return fail_write;
    auto &segment = segments[reinterpret_cast<uintptr_t>(handle)-1];
    assert(!segment.closed);
    auto &output = frame->codec_id == K_MP4_CODEC_ID_G711A ? segment.audio : segment.frames;
    output.push_back({frame->time_stamp,
        std::vector<uint8_t>(frame->data, frame->data + frame->data_length)});
    return 0;
}

static int feed_us(uint64_t pts, const std::vector<uint8_t> &bytes) {
    auto frame = std::make_shared<LawrecEncodedFrame>();
    frame->bytes = bytes; frame->pts_us = pts;
    return record_video_frame(frame, test_pending, test_progress);
}
static int feed(uint64_t ms, const std::vector<uint8_t> &bytes) {
    return feed_us(ms*1000, bytes);
}
const std::vector<uint8_t> headers = {0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xce};
const std::vector<uint8_t> idr = {0,0,0,1,0x65,0x11};
const std::vector<uint8_t> delta = {0,0,0,1,0x41,0x22};
static void begin(uint64_t start_us = 1000000) {
    fail_reserve = false; fail_write = fail_close = 0;
    fail_encoder_stop = fail_audio_stop = fail_end = 0; kept_audio_tail = false;
    close_segment_locked(false);
    segments.clear();
    reset_runtime_fields_locked();
    g_record_audio_queue.reset();
    test_pending.reset(); test_progress = std::chrono::steady_clock::now();
    g_record.config.video_width = 1280; g_record.config.video_height = 720;
    g_record.stop_requested = false; g_record.last_error = 0;
    assert(open_segment_locked() == 0);
    assert(feed_us(start_us, headers) == 0 && segments[0].frames.empty());
    assert(feed_us(start_us, delta) == 0 && segments[0].frames.empty());
    assert(feed_us(start_us, idr) == 0 && segments[0].frames.size() == 1);
}
static void push_audio(uint64_t pts, unsigned bytes = 320) {
    auto packet = std::make_shared<LawrecEncodedFrame>();
    packet->bytes.assign(bytes, 0xd5); packet->pts_us = pts;
    assert(g_record_audio_queue.push(packet) == 0);
}
static void audio_tail_tests() {
    RecordAudioCursor pending;
    auto progress = std::chrono::steady_clock::now();
    auto audio_begin = [&] {
        pending.reset(); begin();
        g_record.audio_enabled = true; g_record.sys_initialized = true;
    };
    audio_begin();
    assert(lawrec_record_stop_async() == 0);
    push_audio(1000000); // A short recording still has a 40-ms audio tail.
    push_audio(1040000); // Future audio must not extend the stopped recording.
    cleanup_record_runtime(&pending, &progress);
    assert(!g_record.last_error && kept_audio_tail);
    assert(segments[0].closed && segments[0].audio.size() == 1);
    assert(segments[0].audio[0].pts == 0);
    assert(segments[0].audio[0].bytes.size() == 267 && pending.offset == 267);
    assert(segments[0].video_end == 33334 && segments[0].audio_end == 33375);
    pending.reset();

    audio_begin();
    g_record.frame_period_us = 66667; // 15-FPS tail needs two 40-ms packets.
    assert(lawrec_record_stop_async() == 0);
    std::thread delayed([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        push_audio(1000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Prove the waiter does not hold the UI/control state mutex.
        assert(lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING);
        push_audio(1040000);
    });
    cleanup_record_runtime(&pending, &progress); delayed.join();
    assert(!g_record.last_error && segments[0].audio.size() == 2);
    assert(segments[0].audio[1].bytes.size() == 214 && segments[0].audio_end == 66750);

    audio_begin();
    g_record.stop_requested = true;
    auto start = std::chrono::steady_clock::now();
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -ETIMEDOUT && !segments[0].published);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));

    audio_begin();
    g_record.stop_requested = true; push_audio(1000000); fail_write = -ENOSPC;
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -ENOSPC && !segments[0].published);
    pending.reset();

    audio_begin();
    g_record.stop_requested = true; push_audio(1000000);
    assert(g_record_audio_queue.fail(-ENOBUFS) == -ENOBUFS);
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -ENOBUFS && !segments[0].published);

    audio_begin();
    g_record.stop_requested = true; push_audio(1000000);
    fail_encoder_stop = -EIO;
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -EIO && !kept_audio_tail && segments[0].audio.empty());

    audio_begin();
    g_record.stop_requested = true; push_audio(1000000);
    fail_audio_stop = -EIO;
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -EIO && !segments[0].published);

    audio_begin();
    g_record.stop_requested = true; g_record.last_error = -ENOSPC;
    push_audio(1000000); fail_audio_stop = -EIO; fail_close = -EROFS;
    cleanup_record_runtime(&pending, &progress);
    assert(g_record.last_error == -ENOSPC && !kept_audio_tail && segments[0].audio.empty());

    audio_begin(); g_record.stop_requested = true;
    push_audio(1040000); // A future packet is not evidence of a complete tail.
    assert(record_audio_tail(1033334, pending, progress) == -ENODATA);
    pending.reset();

    audio_begin(); g_record.stop_requested = true;
    push_audio(1000000, 321);
    assert(record_audio_tail(1033334, pending, progress) == -EINVAL);
    pending.reset();
    close_segment_locked(false);
}

static std::vector<uint8_t> sequence(unsigned index, unsigned count = 320) {
    std::vector<uint8_t> result;
    for (unsigned i = 0; i < count; ++i) result.push_back((index+i)%251);
    return result;
}
static void push_sequence(uint64_t pts, unsigned index) {
    auto packet = std::make_shared<LawrecEncodedFrame>();
    packet->bytes = sequence(index); packet->pts_us = pts;
    assert(g_record_audio_queue.push(packet) == 0);
}
static std::vector<uint8_t> audio_bytes(const Segment &segment) {
    std::vector<uint8_t> bytes;
    for (const auto &sample : segment.audio)
        bytes.insert(bytes.end(), sample.bytes.begin(), sample.bytes.end());
    return bytes;
}
static void audio_segment_tests() {
    begin(1000010); g_record.audio_enabled = true;
    push_sequence(960000, 0); // Entire packet predates the first accepted IDR.
    push_sequence(1000000, 320);
    assert(feed_us(1040010, delta) == 0);
    auto expected = sequence(320); expected.erase(expected.begin());
    assert(audio_bytes(segments[0]) == expected && segments[0].audio[0].pts == 115);

    begin(); g_record.audio_enabled = true;
    for (unsigned i = 0; i < 25; ++i) push_sequence(1000000+i*40000, i*320);
    std::thread late([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            assert(segments.size() == 1 && !segments[0].closed);
        }
        push_sequence(2000000, 8000);
    });
    assert(feed_us(2010010, idr) == 0); late.join();
    assert(segments.size() == 2 && segments[0].published);
    assert(segments[0].audio.back().bytes.size() == 81 && test_pending.offset == 81);
    assert(segments[0].video_end == 1010010 && segments[0].audio_end == 1010125);
    push_sequence(2040000, 8320);
    assert(feed_us(2080000, delta) == 0);
    assert(segments[1].audio[0].pts == 115 && segments[1].audio[0].bytes.size() == 239);
    auto joined = audio_bytes(segments[0]);
    auto next = audio_bytes(segments[1]); joined.insert(joined.end(), next.begin(), next.end());
    assert(joined == sequence(0, 8640)); // No sample loss/duplication across the IDR.

    begin(); g_record.audio_enabled = true;
    assert(feed(2000, idr) == -ETIMEDOUT);
    assert(!segments[0].published && segments.size() == 1 && g_record.stop_requested);

    begin(); g_record.audio_enabled = true;
    push_sequence(2040000, 0); // A clock gap is not repaired by timestamp rewriting.
    assert(feed(2000, idr) == -ENODATA && !segments[0].published);

    begin(); g_record.audio_enabled = true;
    for (unsigned i = 0; i < 24; ++i) push_sequence(1000000+i*40000, i*320);
    std::thread stop_inflight([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(lawrec_record_stop_async() == 0);
        assert(lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING);
        push_sequence(1960000, 7680); push_sequence(2000000, 8000);
    });
    assert(feed_us(2000010, idr) == 0); stop_inflight.join();
    assert(segments.size() == 2 && segments[0].published && segments[1].frames.size() == 1);
    assert(g_record.last_pts_us == 2000010 && lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING);
    assert(feed(2100, delta) == 0 && segments[1].frames.size() == 1);
    cleanup_record_runtime(&test_pending, &test_progress);
    assert(!g_record.last_error && segments[1].audio.size() == 1);

    begin(); fail_end = -EIO;
    assert(feed(2500, idr) == -EIO && g_record.stop_requested && !segments[0].published);

    begin(); g_record.audio_enabled = true;
    push_sequence(UINT64_MAX-100, 0);
    assert(record_audio_tail(1033334, test_pending, test_progress) == -EINVAL);
    begin(); g_record.audio_enabled = true;
    push_sequence(1000000, 0); push_sequence(1000000, 320);
    assert(record_audio_tail(1080000, test_pending, test_progress) == -EINVAL);
    close_segment_locked(false);
}
int main() {
    begin();
    g_record.audio_enabled = true;
    g_record_audio_queue.reset();
    auto audio = std::make_shared<LawrecEncodedFrame>();
    audio->bytes.assign(320, 0xd5);
    audio->pts_us = 1040000;
    assert(g_record_audio_queue.push(audio) == 0);
    RecordAudioCursor pending;
    auto progress = std::chrono::steady_clock::now();
    assert(record_audio_before(1030000, pending, progress) == 0 && pending);
    assert(record_audio_before(1080000, pending, progress) == 0 && !pending);
    assert(segments[0].audio.size() == 1 && segments[0].audio[0].pts == 40000);
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
    begin(); fail_close = -EIO;
    assert(feed(2500, idr) == -EIO && g_record.stop_requested);
    assert(segments.size() == 1 && !segments[0].published);
    begin(); fail_write = -EIO;
    assert(feed(2500, idr) == -EIO && g_record.stop_requested);
    assert(segments[0].published && !segments[1].published);
    begin(); fail_write = -ENOSPC;
    assert(feed(1100, delta) == -ENOSPC && g_record.last_error == -ENOSPC);
    assert(g_record.stop_requested && !segments[0].published);
    fail_write = 0; fail_close = -EROFS;
    assert(close_segment_locked(true) == -EROFS && !segments[0].published);
    begin(); g_record.last_error = -ENOSPC; fail_close = -EROFS;
    cleanup_record_runtime();
    assert(g_record.last_error == -ENOSPC && segments[0].closed && !segments[0].published);
    begin();
    assert(feed(900, delta) == -EINVAL && g_record.stop_requested);
    assert(segments[0].frames.size() == 1);
    begin();
    assert(feed_us(1000500, delta) == 0);
    assert(feed_us(1000400, delta) == -EINVAL); // Same millisecond, reversed capture order.
    begin(); g_record.segment_seconds = 0;
    assert(feed(2500, idr) == 0 && segments.size() == 1);
    g_record.stop_requested = true;
    assert(feed(3500, idr) == 0 && segments[0].frames.size() == 2);
    close_segment_locked(false);
    audio_tail_tests();
    audio_segment_tests();
    puts("record: worker frames, startup clip, delayed IDR audio, byte-continuous split, in-flight stop, timing failures and tail passed");
}
