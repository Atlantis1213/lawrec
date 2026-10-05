# lawrec 功能实现说明

本目录说明当前 `demo` 分支的实际实现。每篇文档都从代码入口、数据流、关键 API、线程边界、错误处理和当前限制几个方面描述功能。

## 文档索引

1. [总体架构与进程边界](00-architecture.md)
2. [摄像头、VICAP、VB 与 VO 预览](01-camera-vicap-vo.md)
3. [MobileRetinaFace、AI2D 与 KPU](02-ai-retinaface.md)
4. [OSD 检测框和关键点](03-osd.md)
5. [RT-Smart IPC 控制](04-ipc-control.md)
6. [共享帧对象、H.264 解析与队列策略](05-frame-queue-and-h264.md)
7. [VENC/AENC 共享媒体源](06-media-source.md)
8. [live555 RTSP 推流](07-rtsp.md)
9. [MP4 录像与 G.711A 音频](08-mp4-recording.md)
10. [LVGL 页面与触控输入](09-ui-touch.md)
11. [状态、指标与数据协议](10-status-metrics.md)
12. [构建、测试和部署](11-build-test-deploy.md)

## 代码入口速查

| 功能 | 主要入口 |
| --- | --- |
| RT-Smart 启动 | `big/main.cpp` 的 `main` |
| 摄像头和显示 | `big/camera.cpp` 的 `Camera::start`、`Camera::set_preview` |
| AI 推理 | `big/detector.cpp` 的 `Detector::load`、`Detector::process` |
| OSD | `big/osd.cpp` 的 `Osd::start`、`Osd::show` |
| RT-Smart IPC | `big/control.cpp` 的 `VisionControl` |
| Linux 媒体后台 | `little/media/main.cpp` 的 `main` |
| 编码源 | `little/media/source.cpp`、`source_frames.cpp` 的 `MediaSource` |
| RTSP | `little/media/rtsp.cpp`、`rtsp_stream.cpp`、`live_source.cpp` |
| 录像 | `little/media/recorder.cpp`、`mp4_sink.cpp` |
| UI | `little/ui/main.cpp`、`page.cpp`、`ctl.cpp` |
| 进程间协议 | `common/protocol.h`、`common/socket.cpp` |
| 共享队列 | `common/frame_queue.cpp`、`common/frame.cpp` |

## 阅读提示

`vision.elf` 负责摄像头、VB、VO、AI 和 OSD；`media_service` 负责 VENC/AENC 的业务控制、编码帧队列、RTSP 和录像；`demo_ui` 负责 LVGL 和触控。三者之间只传控制消息和状态，不通过应用层复制原始摄像头帧。

当前文档描述的是代码实现和离线测试结果。传感器是否真实出帧、LCD 是否显示正确、触控体验、硬件 KPU/AENC 的实际性能和 VLC 主观播放效果，仍以最终上板验收记录为准。
