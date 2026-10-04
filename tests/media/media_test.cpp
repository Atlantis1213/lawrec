// Real SDK muxer + RTSP worker, with only the codec callbacks replaced by a fixture.
#include "recorder.h"
#include "rtsp.h"
#include "config.h"
#include <mov-reader.h>
#include <mov-format.h>
#include <GroupsockHelper.hh>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {
using namespace demo;
std::vector<FramePtr> samples;
std::thread producer;
std::atomic<unsigned> producing{0}, starts{0}, stops{0}, delay_ms{2};
bool silent = false;
void load_sample() {
    std::ifstream input("out/tests/media/sample.h264", std::ios::binary);
    assert(input.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    std::vector<Nal> nals; assert(!split_h264(bytes, nals));
    H264Headers headers; auto current = std::make_shared<Frame>();
    auto append = [&] {
        if (!current->bytes.empty()) {
            assert(headers.prepare(*current) == 1);
            samples.push_back(current); current = std::make_shared<Frame>();
        }
    };
    for (auto nal : nals) {
        if (nal.type == 9) append();
        current->bytes.insert(current->bytes.end(), {0,0,0,1});
        current->bytes.insert(current->bytes.end(), bytes.begin() + nal.offset, bytes.begin() + nal.offset + nal.size);
    }
    append(); assert(samples.size() == 30 && samples.front()->key && samples[15]->key);
}
}
namespace demo {
int MediaSource::attach(Consumer consumer, Feed &feed) {
    std::lock_guard<std::mutex> operation(operations_); unsigned index = unsigned(consumer);
    {
        std::lock_guard<std::mutex> guard(frames_);
        assert(!feeds_[index]); feed.video.reset(); feed.audio.reset(); feeds_[index] = &feed;
        if (owners_) { owners_ |= 1U << index; return 0; }
        owners_ = 1U << index; stats_ = {};
    }
    ++starts; producing = 1;
    producer = std::thread([this] {
        uint64_t audio_pts = 1000000; unsigned i = 0;
        while (producing) {
            if (!silent) {
                auto video = std::make_shared<Frame>(*samples[i % samples.size()]);
                video->pts_us = 1000000 + uint64_t(i) * 1000000 / video_fps; ++i;
                std::lock_guard<std::mutex> guard(frames_);
                for (auto *feed : feeds_) if (feed) feed->video.push(video);
                ++stats_.video_frames; stats_.video_pts = video->pts_us;
                while (audio_pts <= video->pts_us) {
                    auto audio = std::make_shared<Frame>(); audio->bytes.assign(audio_samples, 0xd5); audio->pts_us = audio_pts;
                    for (auto *feed : feeds_) if (feed) feed->audio.push(audio);
                    ++stats_.audio_packets; stats_.audio_pts = audio_pts; audio_pts += 40000;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
        }
    }); return 0;
}
int MediaSource::detach(Consumer consumer, bool drain) {
    std::lock_guard<std::mutex> operation(operations_); unsigned index = unsigned(consumer);
    {
        std::lock_guard<std::mutex> guard(frames_);
        if (!(owners_ & (1U << index))) return 0;
        auto *feed = feeds_[index]; feeds_[index] = nullptr; owners_ &= ~(1U << index);
        if (drain) { feed->video.finish(); feed->audio.finish(); }
        else { feed->video.close(); feed->audio.close(); }
    }
    if (!owners_) { producing = 0; producer.join(); ++stops; }
    return 0;
}
SourceStats MediaSource::stats() const { std::lock_guard<std::mutex> guard(frames_); return stats_; }
bool MediaSource::h264_config(std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) const {
    H264Headers headers; Frame first = *samples.front(); assert(headers.prepare(first) == 1);
    return headers.config(sps, pps);
}
}
namespace {
using namespace demo;
int file_read(void *fp, void *data, uint64_t size) { return fread(data, 1, size, static_cast<FILE *>(fp)) == size ? 0 : -EIO; }
int file_write(void *, const void *, uint64_t) { return -EINVAL; }
int file_seek(void *fp, int64_t offset) { return fseeko(static_cast<FILE *>(fp), offset, offset >= 0 ? SEEK_SET : SEEK_END); }
int64_t file_tell(void *fp) { return ftello(static_cast<FILE *>(fp)); }
struct Readback {
    uint32_t video_track = 0, audio_track = 0;
    unsigned video_count = 0, audio_count = 0;
    uint64_t audio_bytes = 0;
    int64_t video_pts = -1, audio_pts = -1;
    static void video(void *context, uint32_t track, uint32_t scale, uint8_t object, int width, int height, const void *, size_t size) {
        auto &s = *static_cast<Readback *>(context);
        assert(object == MOV_OBJECT_H264 && width == 1280 && height == 720 && scale == 1000 && size > 0);
        s.video_track = track;
    }
    static void audio(void *context, uint32_t track, uint32_t scale, uint8_t object, int channels, int bits, int rate, const void *, size_t) {
        auto &s = *static_cast<Readback *>(context);
        assert(object == MOV_OBJECT_G711a && channels == 1 && bits == 16 && rate == 8000 && scale == 1000);
        s.audio_track = track;
    }
    static void packet(void *context, uint32_t track, const void *bytes, size_t size, int64_t pts, int64_t dts, int flags) {
        auto &s = *static_cast<Readback *>(context); assert(pts == dts);
        if (track == s.video_track) {
            if (!s.video_count) assert(flags & MOV_AV_FLAG_KEYFREAME);
            assert(pts > s.video_pts && size > 4); s.video_pts = pts; ++s.video_count;
        } else {
            assert(track == s.audio_track && pts > s.audio_pts && size > 0);
            for (size_t i = 0; i < size; ++i) assert(static_cast<const uint8_t *>(bytes)[i] == 0xd5);
            s.audio_pts = pts; ++s.audio_count; s.audio_bytes += size;
        }
    }
};
Readback inspect(const char *path, uint64_t max_duration) {
    FILE *file = fopen(path, "rb"); assert(file);
    const mov_buffer_t io{file_read, file_write, file_seek, file_tell};
    auto *reader = mov_reader_create(&io, file); assert(reader && mov_reader_gettrackcount(reader) == 2);
    uint64_t duration = mov_reader_getduration(reader); assert(duration > 0 && duration <= max_duration);
    Readback result; mov_reader_trackinfo_t info{Readback::video, Readback::audio, nullptr};
    assert(!mov_reader_getinfo(reader, 0, &info, &result) && !mov_reader_getinfo(reader, 1, &info, &result));
    std::vector<uint8_t> buffer(max_access_unit + 512); int ret;
    while ((ret = mov_reader_read(reader, buffer.data(), buffer.size(), Readback::packet, &result)) > 0) {}
    assert(!ret && result.video_count && result.audio_count);
    mov_reader_destroy(reader); assert(!fclose(file)); return result;
}
template<class Worker> void wait(Worker &worker, StreamState expected) {
    for (int i = 0; i < 450; ++i) {
        auto status = worker.status();
        if (status.state == expected) return;
        assert(expected == StreamState::Failed || status.state != StreamState::Failed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "media worker deadline");
}
std::string find_clip(const char *directory) {
    DIR *dir = opendir(directory); assert(dir); std::string clip;
    while (auto *entry = readdir(dir)) {
        std::string name(entry->d_name);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".mp4") {
            assert(clip.empty()); clip = std::string(directory) + "/" + name;
        }
        assert(name.size() < 5 || name.substr(name.size() - 5) != ".part");
    }
    closedir(dir); assert(!clip.empty()); return clip;
}
void muxer_edges() {
    Mp4Sink sink; Frame video = *samples.front(); video.pts_us = 1000000;
    int fd = open("out/tests/media/clip.mp4", O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
    assert(!sink.open(fd, video));
    for (unsigned i = 0; i < 15; ++i) {
        video = *samples[i]; video.pts_us = 1000000 + uint64_t(i) * 1000000 / video_fps;
        assert(!sink.video(video));
    }
    Frame audio; audio.bytes.assign(320, 0xd5); audio.pts_us = 990000;
    uint64_t end = 1500000;
    for (; audio.pts_us < end; audio.pts_us += 40000) assert(!sink.audio(audio, end));
    auto stats = sink.stats(); assert(stats.video_frames == 15 && stats.audio_bytes == 4000);
    assert(!sink.close(end));
    auto read = inspect("out/tests/media/clip.mp4", 501);
    assert(read.video_count == 15 && read.audio_bytes == 4000);
    // Real OS write failure must not become a success even during moov finalization.
    Mp4Sink full; fd = open("/dev/full", O_RDWR); assert(fd >= 0);
    video = *samples.front(); video.pts_us = 1000000;
    int ret = full.open(fd, video); assert(ret == -ENOSPC && full.close() == -ENOSPC);
    Mp4Sink finalize;
    fd = open("out/tests/media/tail-error.part", O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
    video = *samples.front(); video.pts_us = 1000000;
    assert(!finalize.open(fd, video) && !finalize.video(video));
    // Linux permits reopening the same descriptor onto a failing device, test-only.
    int bad = open("/dev/full", O_RDWR); assert(bad >= 0 && dup2(bad, fd) == fd); close(bad);
    assert(finalize.close() == -ENOSPC); unlink("out/tests/media/tail-error.part");
}
}
int main() {
    ReceivingInterfaceAddr = SendingInterfaceAddr = htonl(INADDR_LOOPBACK);
    load_sample(); muxer_edges();
    char folder[] = "/tmp/demo-record-XXXXXX"; assert(mkdtemp(folder));
    demo::MediaSource source; RtspWorker rtsp(source); Recorder record(source, folder);
    assert(!rtsp.request(true)); wait(rtsp, StreamState::Running);
    assert(!record.request(true) && !record.request(true)); wait(record, StreamState::Running);
    assert(starts == 1 && stops == 0); // One shared codec start, not two competing encoders.
    assert(!record.request(false)); wait(record, StreamState::Off);
    auto status = record.status(); assert(!status.error && status.written.video_frames && status.written.audio_packets);
    assert(rtsp.status().state == StreamState::Running && producing && !stops);
    auto clip = find_clip(folder); inspect(clip.c_str(), 15001); unlink(clip.c_str());
    // Reverse stop order: recording must survive removing the RTSP owner.
    assert(!record.request(true)); wait(record, StreamState::Running);
    assert(!rtsp.request(false)); wait(rtsp, StreamState::Off);
    assert(record.status().state == StreamState::Running && producing && starts == 1 && stops == 0);
    wait(record, StreamState::Off); // Accelerated callbacks carry 15s of PTS, not a 15s soak.
    status = record.status(); assert(!status.error && starts == 1 && stops == 1);
    clip = find_clip(folder); inspect(clip.c_str(), 15001);
    assert(status.written.video_frames == 450 && status.written.audio_bytes == 120000);
    {
        std::ifstream input(clip, std::ios::binary);
        std::ofstream output("out/tests/media/auto-15s.mp4", std::ios::binary | std::ios::trunc);
        assert(input && output); output << input.rdbuf(); assert(output.good());
    }
    unlink(clip.c_str());
    assert(!record.shutdown() && !rtsp.shutdown());
    silent = true; assert(!record.request(true)); wait(record, StreamState::Failed);
    assert(record.status().error == -ETIMEDOUT && starts == 2 && stops == 2);
    assert(!record.shutdown() && !rmdir(folder));
    std::puts("real SDK MP4: H264/PCMA tracks, monotonic shared PTS, sample clipping, final duration, write/close errors; RTSP/record ownership and both stop orders passed (fake codecs only)");
}
