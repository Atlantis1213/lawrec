# MP4 录像与 G.711A 音频

## 1. 入口和职责

录像由 `little/media/recorder.cpp` 的 `Recorder` 管理，文件写入由 `little/media/mp4_sink.cpp` 的 `Mp4Sink` 管理。Recorder 负责线程、队列、开始/停止和文件路径；Mp4Sink 负责 SDK libmov writer 和样本封装。

## 2. 开始录像

`Recorder::request(true)` 检查状态、清零上一段统计并创建 worker。worker：

1. 在 `/sharefs/lawrec_records` 创建带 UTC 时间和 `mkstemps` 后缀的 `.mp4.part` 临时文件。
2. attach `Consumer::Record`，启动或复用 MediaSource。
3. 等待第一个完整 key frame。
4. 以该 IDR PTS 为 recording start，设置 15 秒 source PTS limit。
5. `Mp4Sink::open()` 解析 SPS/PPS、创建 MOV writer、添加 H264 和 G.711A track。
6. 写入至少一个 video sample 和一个 audio packet 后才把 state 改为 Running。

3 秒内不能同时得到视频和音频时录像失败并清理临时文件。

## 3. H.264 MP4 封装

`Mp4Sink::open()` 从首个 IDR 提取 SPS/PPS，生成 AVCDecoderConfigurationRecord，添加 1280×720 H264 track，并设置 in-band AVC 写法；随后添加 8 kHz mono G.711A track。

每个 video Frame 的 Annex-B NAL 被转换成 AVCC sample：每个 NAL 前写 4-byte big-endian length，再拼接 payload。PTS 从首帧基准转换为毫秒，DTS 使用相同值，因为固定编码不含 B frame 重排。

## 4. G.711A 音频写入

音频来自 MediaSource 的 8 kHz、320-byte packet。`clip_audio()` 根据录像起始 PTS 和 video end PTS 选择样本，按 125 µs 样本边界裁剪，避免音频任意超前或拖尾。`Mp4Sink::audio()` 更新 `audio_end_pts`，Recorder 等待能落在 video window 内的音频。

## 5. 停止与收尾

停止请求把 state 设为 Stopping。worker：

1. detach Record consumer；正常停止使用 drain 读取已接受的尾帧。
2. 写入未消费 video queue，直到 recording limit。
3. 按 video end PTS 裁剪和写入剩余 audio。
4. 检查至少存在 video/audio sample。
5. 调用 `demo_mov_writer_end_track()` 设置两条 track 的结束时间。
6. destroy writer，flush、检查 ferror 并 fclose。
7. 只有全部成功才把 `.mp4.part` rename 为 `.mp4`。

自动录制在 source PTS 到达 15 秒时结束，墙上时间可能不同；`request(false)` 不依赖新的 vision ACK。

## 6. SDK MP4 适配

`tools/mp4.cmake` 从 SDK source 生成私有构建副本，应用 `patches/mp4-end-time.cmake` 补充明确 track end time，应用 `patches/mp4-inband-avc.cmake` 配置 in-band AVC。不修改 SDK 原始源码、安装 archive、panel 或 touch adapter，构建结果放在 `out/`。

## 7. 文件和错误策略

只有 Recorder worker 写文件，codec callback 和 UI 不写 MP4。write/seek/tell/flush/close 错误由 `Mp4Sink::remember()` 保存并在 cleanup 后返回。`.part` 只表示未完成收尾；失败不报告成功。当前不实现断电恢复、fast-start relocation、板端回放和自动恢复。

## 8. 验证边界

`tests/media/media_test.cpp` 使用真实 libmov 副本和模拟 H264/PCMA 输入验证 track、PTS、截尾、写入错误、双 consumer 生命周期和 stop order。FFmpeg 可检查文件结构和解码，但不能证明真实 K230 AENC、音画同步或 SD 卡长期写入性能。
