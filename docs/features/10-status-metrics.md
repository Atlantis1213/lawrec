# 状态、指标与数据协议

## 1. Status 字段

`common/protocol.h` 的 `Status` 固定为 84 bytes：

- flags：Preview、AI、RTSP、Record。
- busy：正在处理的命令位。
- detections：最近检测数量。
- ai2d_us/kpu_us/post_us/total_us：最近一次 AI timing。
- ai_fps_milli/video_fps_milli：milli-FPS。
- bitrate_kbps：媒体后台按 callback bytes 计算的码率。
- video_queue/audio_queue：RTSP 与 Record 队列当前深度之和。
- rss_kib/cpu_percent_milli：media_service 指标。
- vb_kib：vision 上报的 VB 配置预算。
- last_error：最近错误。

不可用指标使用 `UINT32_MAX`，UI 显示 `--`，不把不可用当作 0。

## 2. Vision 侧 AI 统计

`big/main.cpp` 每次成功 Detector process 后更新 timing 和 detections。经过至少 1 秒时，用完成推理帧数除以 monotonic elapsed 计算 `ai_fps_milli`。这是完成推理的速度，不是理论摄像头 FPS。Status 保存最近一次 timing；P50/P95 需要额外诊断聚合，不能从最近值推断。

## 3. Media 侧视频统计

`MediaMetrics::update()` 每秒读取 `MediaSource::SourceStats`：video frame delta 得 FPS，video byte delta 得 kbps。随后从 `/proc/self/stat` 的 field 14/15 读取 media_service user+system ticks，field 24 读取 resident pages。首次 CPU 采样没有基线，因此不可用。

CPU 指标只代表 media_service，100% 表示一个 CPU 满载，不包含 UI、RT-Smart 或其他进程。RSS、VB 预算和整板内存不是同一个概念。

## 4. 队列指标

每个 `FrameQueue` 保存当前 depth/bytes、peak_bytes、accepted/popped/dropped 和 sticky error。RTSP、Recorder 队列独立；Status 中的 queue 字段是两者 depth 相加，详细诊断应读取各自状态。

## 5. 时间基准

AI 和 timeout 用 `steady_clock`；音视频内容使用设备 PTS；RTP presentation time 用 `gettimeofday()` 通过 `MediaClock` 从 PTS 映射。不能用 UI 轮询间隔代替 AI/编码耗时，也不能把设备 callback 次数当作客户端收包数。

## 6. 验证边界

离线 RTSP 测试用真实 live555 和模拟 codec 验证 SDP、RTP、H264 access unit marker、PCMA payload、PTS、stop/port release；MP4 使用 ffprobe/ffmpeg 检查轨道、时长和解码错误。这些测试不证明真实摄像头、KPU、AENC、VICAP 并行或 LCD。
