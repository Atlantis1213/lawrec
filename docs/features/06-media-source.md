# VENC/AENC 共享媒体源

## 1. 入口和职责

实现位于 `little/media/source.h`、`source.cpp`、`source_frames.cpp`。`MediaSource` 是 Linux 媒体后台唯一的编码源拥有者：VENC0 绑定 RT-Smart VICAP CHN2，AENC0 绑定内部 Codec 的 AI handle，然后把 callback 数据复制成不可变 `Frame`，投递到 RTSP 和 Recorder 各自的 `Feed`。

消费者只有 `Consumer::Rtsp` 和 `Consumer::Record`。

## 2. 启动编码和音频

第一个 consumer attach 时执行 `MediaSource::start()`。

### 视频

- `K_PT_H264`、High profile。
- 1280×720，输入/输出 30 FPS。
- CBR 4000 kbps，GOP 30。
- 使用 `stream_buffers` 和 `stream_block_bytes`。
- 初始化 VENC0、启用 IDR、注册 callback、start、绑定 `encode_channel=2`。

### 音频

- I2S input，16 bit，8 kHz。
- 双物理 slot，选择 mono-right。
- 每帧 320 samples，约 40 ms。
- 内部 Codec input。
- AENC0 类型 `K_PT_G711A`，注册 callback、start、AI→AENC0 bind。

最后请求一次 VENC IDR，保证消费者尽快得到可解码起点。

## 3. 回调数据复制

视频 callback 检查 channel、pack count、pack 类型、各 pack 地址/长度和总大小不超过 1 MiB；非 header pack 必须是 I/P，多个 pack PTS 必须一致。随后复制所有 pack 到一个 Frame，并调用 `H264Headers::prepare()`。

音频 callback 检查 stream 非空、长度为 320 bytes、PTS 单调递增，再复制为 audio Frame。

回调返回后不再引用 SDK 临时 buffer，所以 RTSP/录像可异步消费自有数据。

## 4. H.264 头和访问单元

`common/frame.cpp` 的 `split_h264()` 支持 3/4-byte Annex-B start code，限制单个 access unit 不超过 1 MiB、NAL 不超过 128 个，并拒绝非法 type、空 payload 和 forbidden bit。

`H264Headers::prepare()` 缓存 SPS/PPS，识别 VCL/IDR，并在已有 SPS/PPS 的 IDR 前插入 parameter sets，以 `Frame::key` 标记完整可解码起点。参数集变化时清理不匹配的缓存，避免旧 SPS 与新视频混用。

## 5. 独立 Feed 和队列策略

RTSP 和录像各有 video/audio 队列，绝不竞争同一个 queue：

- video：最多 90 帧或 8 MiB。
- audio：最多 100 包或 128 KiB。

队列统计 accepted、popped、dropped、深度、峰值字节和错误。RTSP video 满时丢弃 backlog、重新等 IDR，并由 `MediaSource::tick()` 在 callback 外请求 IDR；录像 video 满时返回 `-ENOBUFS`，录像失败；Live audio 满时丢弃旧包以追赶实时数据。

## 6. Attach/detach 生命周期

第一个 consumer attach 清理上一代统计、reset queue、启动 VENC/AENC；第二个 attach 复用 codec，只请求新的 IDR。

detach 时 RTSP 通常 close queue，Recorder 的正常停止使用 drain/finish；最后一个 owner 消失才 cleanup codec。SDK stop/unregister 不在 callback 锁内调用，因为它们可能等待 callback 返回。cleanup error 会被保留，后续 attach 拒绝。

## 7. 统计边界

`SourceStats.video_frames/audio_packets` 是 SDK callback 已接收并通过校验的数据，不代表 VLC 已收到 RTP，也不代表 MP4 已成功写入。客户端统计必须单独采集。
