# live555 RTSP 推流

## 1. 入口

实现位于 `little/media/rtsp.cpp`、`rtsp_stream.cpp`、`live_source.cpp`。`RtspWorker` 创建独立线程；live555 scheduler、environment、server 和 session 都在这个线程中使用。

## 2. 启动流程

`request(true)` 检查当前 state，清理上次状态并启动 worker。worker：

1. 创建 `BasicTaskScheduler` 和 `BasicUsageEnvironment`。
2. 用 `IPv4Server::create()` 绑定 IPv4 8554，端口占用时在申请 codec 前失败。
3. attach `Consumer::Rtsp`。
4. 最多等待 3 秒，直到 video/audio queue 有数据并取得 SPS/PPS。
5. 设置 `OutPacketBuffer::maxSize` 为 1 MiB。
6. 创建包含 video/audio 两个 subsession 的 `ServerMediaSession`。
7. state 从 Starting 变为 Running，进入 live555 event loop。

Running 表示 server、source 和首个媒体数据准备好，不表示 VLC 已连接或主观播放正常。

## 3. 视频 track

新客户端请求视频时，`Track::createNewStreamSource()` 清理旧 backlog，丢弃旧音频 backlog，请求一个 VENC IDR，创建 `LiveSource(video=true)` 和 `AccessUnitFramer`。

`AccessUnitFramer` 继承 `H264VideoStreamDiscreteFramer`，覆盖 `nalUnitEndsAccessUnit()`。SDK 一个 frame 可能包含多个 slice，只有源对象确认最后一个 NAL 时才结束 RTP access unit，避免每个 slice 被当成一帧。

sink 使用 `H264VideoRTPSink::createNew()`，传入缓存的 SPS/PPS。

## 4. 音频 track

音频 track 创建 `LiveSource(video=false)`，每次取一个 G.711A packet。sink 使用 `SimpleRTPSink`：payload type 8、8 kHz、codec name PCMA、单声道。SDK 的 `K_PT_G711A` 对应 RTSP 的 PCMA，不能写成 PCMU。

## 5. LiveSource 到 RTP

`LiveSource::deliver()` 在 live555 等待数据时从 FrameQueue 取 frame；视频用 `split_h264()` 切 NAL，所有 NAL 保持同一个 Frame PTS；音频整包发送。`MediaClock` 把设备 PTS 映射为 wall-clock presentation time，检查目标缓冲区大小并禁止截断。最后一个视频 NAL 才释放 Frame 并调用 `afterGetting()`。

视频完整 access unit duration 约为 1/30 秒，中间 slice 为 0；音频 duration 按 packet 字节数和 8 kHz 计算。

## 6. 事件循环和错误

`Reactor::poll()` 每 20 ms 检查 MediaSource/delivery/queue error，以及连续 3 秒无 video/audio 数据。错误会退出 event loop，关闭 live555 对象并 detach RTSP consumer。停止过程在 callback 外完成 codec cleanup。

RTSP queue 拥塞时丢完整 access unit/GOP 并请求 IDR；新客户端从新鲜 IDR 开始，录像 queue 不受影响。

## 7. 验证边界

`tests/rtsp/rtsp_test.cpp` 使用真实 live555、模拟 codec、DESCRIBE/SETUP/PLAY 和 TCP interleaved RTP 验证 SDP、H264 NAL marker、PCMA packet、PTS、stop/port release。它不证明真实 GC2093、KPU、硬件音频或 VLC 主观播放结果。
