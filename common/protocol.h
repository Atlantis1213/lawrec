#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace demo {
// Both SDK targets are little-endian; all fields are 32-bit, no pointers/padding.
constexpr uint32_t magic = 0x4c564431, version = 2;
constexpr uint32_t metric_unavailable = UINT32_MAX;
constexpr uint32_t vision_module = 0x56495331;
enum class Command : uint32_t { GetStatus = 0, SetPreview, SetAi, SetRtsp, SetRecord };
enum Flag : uint32_t { Preview = 1, Ai = 2, Rtsp = 4, Record = 8 };
struct Request {
    uint32_t magic_tag = magic, protocol_version = version, bytes = 24;
    uint32_t id = 0, command = 0, value = 0;
};
struct Status {
    uint32_t magic_tag = magic, protocol_version = version, bytes = 84, id = 0;
    int32_t result = 0;
    uint32_t flags = 0, busy = 0, detections = 0;
    uint32_t ai2d_us = 0, kpu_us = 0, post_us = 0, total_us = 0;
    uint32_t ai_fps_milli = 0, video_fps_milli = 0, bitrate_kbps = 0;
    uint32_t video_queue = 0, audio_queue = 0, rss_kib = metric_unavailable, vb_kib = 0;
    int32_t last_error = 0;
    uint32_t cpu_percent_milli = metric_unavailable;
};
static_assert(sizeof(Request) == 24 && sizeof(Status) == 84, "Wire layout drift");
static_assert(std::is_trivially_copyable<Status>::value, "Wire must be plain data");
inline bool decode_request(const void *body, size_t size, Request &request) {
    if (!body || size != sizeof(Request)) return false;
    std::memcpy(&request, body, size);
    return request.magic_tag == magic && request.protocol_version == version &&
           request.bytes == sizeof(Request) && request.command <= 4 &&
           (request.command == 0 ? request.value == 0 : request.value <= 1);
}
inline bool valid_status(const Status &status, uint32_t id) {
    return status.magic_tag == magic && status.protocol_version == version &&
           status.bytes == sizeof(Status) && status.id == id &&
           !(status.flags & ~15U) && !(status.busy & ~15U);
}
}
