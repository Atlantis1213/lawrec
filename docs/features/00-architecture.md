# 总体架构与进程边界

## 1. 三个运行程序

### `vision.elf`

运行在 RT-Smart。入口是 `big/main.cpp`，启动顺序为：

1. 处理 `--build-info` 或模型路径参数。
2. 调用 `kd_mapi_sys_init()` 初始化 MAPI 系统服务。
3. 创建 `Camera`、`Osd`、`VisionControl` 和 `Detector` 对象。
4. 启动 VB、连接器/VO、VICAP 三个通道和 VO 帧结束同步。
5. 加载 RetinaFace 模型，创建 OSD 层和 RT-Smart IPC 服务。
6. 主循环处理控制请求，并在 AI 开关打开时执行推理。
7. 收到 `q` 或信号后按 IPC、模型、OSD、摄像头、MAPI 的顺序退出。

视觉程序持有硬件资源的唯一所有权。Linux 程序不能重复初始化 VB、摄像头、连接器或 VO。

### `media_service`

运行在 Linux。入口是 `little/media/main.cpp`，负责：

- 创建 `/var/run/lawrec-demo.sock` Unix `SOCK_SEQPACKET` 控制 socket。
- 使用文件锁保证同一路径只有一个后台实例。
- 连接 RT-Smart 的 `VisionControl`。
- 创建一个 `MediaSource`，按需启动 VENC0/AENC0。
- 创建 `RtspWorker` 和 `Recorder`，分别消费媒体源的独立队列。
- 每秒读取媒体帧计数和 `/proc/self/stat`，将摘要放入状态响应。

RTSP 和录像在同一个 Linux 进程中，因为两者需要共享同一套编码源和音频源；通过 `shared_ptr<const Frame>` 和两个独立队列完成解耦。

### `demo_ui`

运行在 Linux。入口是 `little/ui/main.cpp`，负责：

- 初始化 LVGL、显示 port 和触控输入 port。
- 创建一个 480×800 竖屏主页面。
- 在 LVGL 主循环中绘制页面和处理事件。
- 用通信线程定期向 `media_service` 查询状态。
- 点击按钮时只记录待发送命令，通信线程负责 socket 往返。

UI 不在按钮回调中直接调用 IPC 或 MAPI，避免跨进程请求阻塞 LVGL 绘制线程。

## 2. 数据流

```text
GC2093
  └─ VICAP CHN0 YUV 800×480 ── VI0→VO1 绑定 ── LCD 预览
  └─ VICAP CHN1 RGB 1280×720 ── Detector::process ── AI2D/KPU ── OSD2
  └─ VICAP CHN2 YUV 1280×720 ── VENC0 ── MediaSource
                                      ├─ RTSP video queue
                                      └─ MP4 video queue
音频 I2S 内部 Codec ── AI/AENC0 ── MediaSource
                                      ├─ RTSP audio queue
                                      └─ MP4 audio queue
```

应用层 IPC 只传 `Request` 和 `Status`。视频帧留在 SDK MAPI 回调和 Linux 进程内，不经过 UI socket。

## 3. 资源归属

| 资源 | 归属 | 原因 |
| --- | --- | --- |
| VB pool、GC2093、VICAP | `vision.elf` 的 `Camera` | 统一配置三个通道和生命周期 |
| VO layer、VO frame-end 位 | `Camera`/`VoSync` | 避免 UI 或诊断程序重复改变显示硬件 |
| RetinaFace 模型和 KPU tensor | `Detector` | 模型只加载一次，主循环串行使用 |
| OSD2、ARGB buffer | `Osd` | 双缓冲和 VO 扫描生命周期统一管理 |
| VENC0/AENC0 | `MediaSource` | RTSP/录像共享同一套编码源 |
| RTSP session | `RtspWorker` | live555 事件循环独立于 UI |
| MP4 writer | `Recorder`/`Mp4Sink` | 只有录像线程写和关闭文件 |
| LVGL 对象和 evdev | `demo_ui` | 触控输入只由 UI port 消费 |

## 4. 启停关系

启动时先启动 `vision.elf`，再启动 `media_service`，最后启动 `demo_ui`。媒体后台连接 RT-Smart 后才能处理 AI、预览和媒体命令；UI 即使后台未连接也可以启动，但按钮会显示连接错误。

停止时先停止 UI，再停止媒体后台，最后通过串口给 `vision.elf` 发送 `q`。媒体后台停止前需要让 RTSP worker 关闭 live555 对象、让 Recorder 完成 MP4 收尾，再释放编码源。

## 5. 错误边界

- AI 失败时只关闭 AI 并清除 OSD，摄像头和编码源保持运行。
- RTSP 失败时只关闭 RTSP consumer，录像 consumer 不被强制关闭。
- 录像写入失败时停止录像并保留错误，RTSP 继续使用自己的队列。
- UI socket 失败时显示后台未连接，不直接重启硬件。
- SDK 清理失败时保留错误状态，避免在回调可能仍被调用时销毁对象。

这不是自动恢复框架；错误会显示和记录，恢复由明确的人工重启或再次请求完成。
