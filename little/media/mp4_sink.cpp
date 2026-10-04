#include "mp4_sink.h"
#include "config.h"
#include <mpeg4-avc.h>
#include <mov-format.h>
#include <cerrno>
#include <limits>
#include <unistd.h>
extern "C" int demo_mov_writer_end_track(mov_writer_t *, int, int64_t);
extern "C" int demo_mov_writer_inband_avc(mov_writer_t *, int);

namespace demo {
int Mp4Sink::remember(int error) {
    if (error && !error_) error_ = error < 0 ? error : -EIO;
    return error_;
}
int Mp4Sink::read(void *context, void *data, uint64_t bytes) {
    auto &sink = *static_cast<Mp4Sink *>(context);
    if (bytes > SIZE_MAX || fread(data, 1, bytes, sink.file_) != bytes) return sink.remember(-EIO);
    return 0;
}
int Mp4Sink::write(void *context, const void *data, uint64_t bytes) {
    auto &sink = *static_cast<Mp4Sink *>(context);
    if (sink.error_) return sink.error_;
    errno = 0;
    if (bytes > SIZE_MAX || fwrite(data, 1, bytes, sink.file_) != bytes)
        return sink.remember(errno ? -errno : -EIO);
    return 0;
}
int Mp4Sink::seek(void *context, int64_t offset) {
    auto &sink = *static_cast<Mp4Sink *>(context);
    if (fseeko(sink.file_, offset, offset >= 0 ? SEEK_SET : SEEK_END)) return sink.remember(-errno);
    return 0;
}
int64_t Mp4Sink::tell(void *context) {
    auto &sink = *static_cast<Mp4Sink *>(context);
    auto offset = ftello(sink.file_);
    if (offset < 0) sink.remember(-errno);
    return offset;
}
int Mp4Sink::open(int fd, const Frame &first) {
    if (file_ || writer_) { if (fd >= 0) ::close(fd); return -EBUSY; }
    file_ = fdopen(fd, "wb+");
    if (!file_) { int error = errno; if (fd >= 0) ::close(fd); return -error; }
    // Make every callback reflect OS write errors; no hidden stdio tail buffer.
    if (setvbuf(file_, nullptr, _IONBF, 0)) return remember(-EIO);
    error_ = 0; stats_ = {}; start_pts_ = first.pts_us;
    std::vector<Nal> nals;
    int ret = split_h264(first.bytes, nals);
    if (ret || !first.key) return remember(ret ? ret : -EBADMSG);
    bool idr = false;
    for (auto nal : nals) {
        if (nal.type == 5) idr = true;
        if (nal.type == 7 || nal.type == 8) {
            if (nal.size > 32768 || nal.size < (nal.type == 7 ? 4U : 2U)) return remember(-EBADMSG);
            auto &header = nal.type == 7 ? sps_ : pps_;
            header.assign(first.bytes.begin() + nal.offset, first.bytes.begin() + nal.offset + nal.size);
        }
    }
    if (!idr || sps_.empty() || pps_.empty()) return remember(-EBADMSG);
    mpeg4_avc_t config{};
    config.profile = sps_[1]; config.compatibility = sps_[2]; config.level = sps_[3]; config.nalu = 4;
    config.chroma_format_idc = 1; // Fixed encoder input: 8-bit YUV420.
    config.nb_sps = config.nb_pps = 1;
    config.sps[0] = {uint16_t(sps_.size()), sps_.data()}; config.pps[0] = {uint16_t(pps_.size()), pps_.data()};
    std::vector<uint8_t> extra(sps_.size() + pps_.size() + 32);
    int size = mpeg4_avc_decoder_configuration_record_save(&config, extra.data(), extra.size());
    if (size <= 0) return remember(-EBADMSG);
    static const mov_buffer_t io{read, write, seek, tell};
    writer_ = mov_writer_create(&io, this, 0); // moov at tail, no FASTSTART file rewrite.
    if (!writer_) return remember(-ENOMEM);
    if (error_) return error_;
    video_track_ = mov_writer_add_video(writer_, MOV_OBJECT_H264, video_width, video_height, extra.data(), size);
    if (video_track_ < 0) return remember(video_track_);
    ret = demo_mov_writer_inband_avc(writer_, video_track_);
    if (ret) return remember(ret);
    audio_track_ = mov_writer_add_audio(writer_, MOV_OBJECT_G711a, 1, 16, audio_rate, "", 0);
    return audio_track_ < 0 ? remember(audio_track_) : error_;
}
int Mp4Sink::video(const Frame &frame) {
    if (error_) return error_;
    if (!writer_ || frame.pts_us < start_pts_ ||
        (stats_.video_frames && frame.pts_us <= stats_.last_video_pts)) return remember(-ERANGE);
    std::vector<Nal> nals; int ret = split_h264(frame.bytes, nals);
    if (ret) return remember(ret);
    sample_.clear(); bool vcl = false;
    for (auto nal : nals) {
        if (nal.type == 7 || nal.type == 8) {
            auto &header = nal.type == 7 ? sps_ : pps_;
            if (nal.size != header.size() || !std::equal(header.begin(), header.end(), frame.bytes.begin() + nal.offset)) {
                std::fprintf(stderr, "[mp4] parameter set changed type=%u old=%zu new=%zu pts=%llu\n",
                    nal.type, header.size(), nal.size, (unsigned long long)frame.pts_us);
                header.assign(frame.bytes.begin() + nal.offset, frame.bytes.begin() + nal.offset + nal.size);
            }
            // avc3 carries each parameter set with the slices that reference it.
            // Encoder dimensions stay fixed by MediaSource, not by SPS byte identity.
        }
        if (nal.type >= 1 && nal.type <= 5) vcl = true;
        for (int shift : {24, 16, 8, 0}) sample_.push_back(uint8_t(nal.size >> shift));
        sample_.insert(sample_.end(), frame.bytes.begin() + nal.offset, frame.bytes.begin() + nal.offset + nal.size);
    }
    if (!vcl) return remember(-EBADMSG);
    uint64_t pts = (frame.pts_us - start_pts_) / 1000;
    if (pts > uint64_t(std::numeric_limits<int64_t>::max())) return remember(-ERANGE);
    if (stats_.video_frames && pts <= (stats_.last_video_pts - start_pts_) / 1000) return remember(-ERANGE);
    ret = mov_writer_write(writer_, video_track_, sample_.data(), sample_.size(), pts, pts,
                           frame.key ? MOV_AV_FLAG_KEYFREAME : 0);
    if (remember(ret)) return error_;
    ++stats_.video_frames; stats_.video_bytes += sample_.size(); stats_.last_video_pts = frame.pts_us;
    return 0;
}
int Mp4Sink::audio(const Frame &frame, uint64_t end_pts) {
    if (error_) return error_;
    auto clip = clip_audio(frame, start_pts_, end_pts);
    if (!clip.samples) return 0;
    if (!writer_ || (stats_.audio_packets && clip.pts_us < stats_.audio_end_pts)) return remember(-ERANGE);
    uint64_t pts = (clip.pts_us - start_pts_) / 1000;
    if (pts > uint64_t(std::numeric_limits<int64_t>::max())) return remember(-ERANGE);
    int ret = mov_writer_write(writer_, audio_track_, frame.bytes.data() + clip.offset, clip.samples, pts, pts, 0);
    if (remember(ret)) return error_;
    ++stats_.audio_packets; stats_.audio_bytes += clip.samples;
    stats_.audio_end_pts = clip.pts_us + clip.samples * (1000000 / audio_rate);
    return 0;
}
int Mp4Sink::close(uint64_t video_end_pts) {
    if (writer_) {
        if (video_end_pts && stats_.video_frames && stats_.audio_packets && !error_) {
            if (video_end_pts <= start_pts_) remember(-ERANGE);
            else {
                remember(demo_mov_writer_end_track(writer_, video_track_, (video_end_pts - start_pts_) / 1000));
                uint64_t audio_duration = stats_.audio_end_pts - start_pts_;
                remember(demo_mov_writer_end_track(writer_, audio_track_, audio_duration / 1000 + (audio_duration % 1000 != 0)));
            }
        }
        mov_writer_destroy(writer_); writer_ = nullptr;
    }
    if (file_) {
        if (fflush(file_) || ferror(file_)) remember(errno ? -errno : -EIO);
        if (fclose(file_)) remember(errno ? -errno : -EIO);
        file_ = nullptr;
    }
    return error_;
}
}
