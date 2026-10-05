# 共享帧对象、H.264 解析与队列策略

## 1. 共享 Frame 对象

`common/frame.h` 定义：

```cpp
struct Frame {
    std::vector<uint8_t> bytes;
    uint64_t pts_us;
    bool key;
};
using FramePtr = std::shared_ptr<const Frame>;
```

MediaSource 在 SDK callback 中创建和填充可变 Frame，push 前转换为 `shared_ptr<const Frame>`。RTSP 和 Recorder 可以引用同一份不可变字节，不会互相修改，也不会在 SDK callback 返回后继续引用 SDK buffer。

## 2. Annex-B 解析

`split_h264()` 从当前位置识别 3-byte 或 4-byte start code，扫描到下一个 start code，去掉末尾 padding zero，输出 `Nal{offset,size,type}`。

边界检查包括：

- access unit 非空且不超过 `max_access_unit=1 MiB`。
- NAL 数不超过 128。
- payload 非空、forbidden bit 为 0。
- NAL type 必须在有效范围内。

解析函数不复制 payload，只返回原始 vector 内的 offset/size；真正跨线程的数据复制发生在 MediaSource callback。

## 3. SPS/PPS 和 IDR

`H264Headers::prepare()` 遍历一个 Frame 的 NAL：

1. type 7 更新 SPS，type 8 更新 PPS。
2. type 1～5 表示存在 VCL；type 5 表示 IDR。
3. SPS 变化时先清除旧 PPS，避免参数集混用。
4. 识别到带 SPS/PPS 的 IDR 时，在 frame bytes 前插入 Annex-B SPS/PPS，并设置 `key=true`。
5. 只有存在 VCL 的 frame 才返回可投递结果；只有 header 的 frame 被统计为参数数据，不进入 consumer queue。

IDR 带完整 parameter set 是 RTSP 新客户端和录像新文件的起点。

## 4. FrameQueue 状态

`common/frame_queue.cpp` 的 `FrameQueue` 用 mutex、condition variable 和 deque 实现。每个队列记录当前深度、字节数、峰值、accepted/popped/dropped 和 sticky error。

- `reset()`：清空旧数据，打开队列，video 进入 waiting-IDR 状态。
- `push()`：校验 Frame 和容量；video 未等到 IDR 时丢弃非 key frame。
- `pop()`：按 timeout 等待数据；成功弹出一帧并更新统计。
- `finish()`：关闭新数据但保留已接受尾帧，适用于录像正常收尾。
- `close()`：关闭并立即丢弃 backlog，适用于 RTSP stop。
- `discard()`：只清空当前 backlog。
- `await_idr()`：清空并让 video 重新等待 key frame。
- `fail()`：保存第一个错误、关闭队列并唤醒消费者。

## 5. 拥塞策略

队列满时按 policy 处理：

### Record

返回 `-ENOBUFS` 并将错误设为 sticky。录像不静默丢帧，否则生成的 MP4 可能报告成功但内容缺失。

### LiveVideo

清空旧 video backlog，重新等待 IDR。当前 frame 如果不是 IDR 也丢弃；如果是 IDR 则作为新的 GOP 起点。MediaSource 通过原子标记让主循环在 callback 外请求 VENC IDR。

### LiveAudio

丢弃队头旧 audio packet，保留最新数据以追赶实时视频；audio 不触发完整 GOP 重置。

## 6. 时间映射和音频裁剪

`common/media_time.h` 的 `MediaClock` 将 source PTS 映射到一个共享 wall-clock epoch，RTSP video/audio 使用同一映射。`clip_audio()` 假定 G.711A 每字节一个样本、8 kHz、每样本 125 µs，在录像 start/end PTS 窗口内按整样本边界返回 offset/count。

这保证 RTSP 两轨和 MP4 两轨使用同一源时间轴；真实 SDK 的首个音频 PTS 语义和板级时钟对齐仍需硬件验收。

## 7. 为什么不用一个队列

RTSP 消费速度会随网络客户端变化，录像写盘速度也会变化。如果两者竞争同一个 queue，一个消费者取走帧后另一个就会缺帧。因此 source 为每个 consumer 建立独立 video/audio queue，只共享不可变 Frame 的内存。
