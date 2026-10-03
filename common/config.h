#pragma once
#include <cstdint>

namespace demo {
constexpr unsigned preview_channel = 0, rgb_channel = 1, encode_channel = 2;
constexpr unsigned preview_width = 800, preview_height = 480;
constexpr unsigned video_width = 1280, video_height = 720, video_fps = 30;
constexpr unsigned video_bitrate_kbps = 4000;
constexpr unsigned capture_buffers = 5;
constexpr unsigned stream_buffers = 30;
constexpr unsigned stream_block_bytes = (video_width * video_height * 3 / 4 + 4095) & ~4095U;
constexpr unsigned rtsp_port = 8554;
constexpr const char *rtsp_name = "lawrec";
constexpr const char *ipc_service = "lawrec_demo";
constexpr unsigned ipc_port = 102;
constexpr const char *socket_path = "/var/run/lawrec-demo.sock";
constexpr const char *record_directory = "/sharefs/lawrec_records";
}
