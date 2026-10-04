# Demo 验收状态

## 离线检查

- [x] 三个业务程序在 K230 SDK Docker 内交叉构建；ELF、加载区间及固定适配代码校验。
- [x] 控制协议、Unix socket、命令 ACK、错误保留和异常消息检查。
- [x] 摄像头/缓冲区资源归属、预览绑定开关及停止顺序的针对性模拟检查。
- [x] AI2D/KPU 集成、模型输出、先验框、NMS、坐标及 OSD 缓冲归属检查。
- [x] IPC 待连接返回值、真实失败释放及 MAPI 等待可取消检查。
- [x] 编码回调复制、独立队列、PTS、首末消费者及资源清理检查。
- [x] live555 TCP H.264/G.711A 回环接收、RTP 标记、大小和超时检查。
- [x] 真实 x264 样本、SDK MP4 封装/回读及独立 FFmpeg 解码；包括 SPS/PPS ID 改变、15 秒自动结束和消费者停止顺序。
- [x] 单页中文 LVGL 渲染、字号、触摸目标、透明预览区、详情弹窗及忙碌/错误交互检查。
- [x] 应用包运行库依赖、模型哈希和限定进程身份的启动/停止脚本检查。

离线模拟检查不是开发板或视觉验收。构建及录像解码均通过 `tools/build.sh`
在 Docker 内完成，未修改 SDK 或冻结显示/触控代码。

## 板端短验收：2026-10-04

设备：小核 `root@192.168.123.74`，大核 COM5。使用现有
27 MHz / 4 lane / ST7701 / VTTH=1 固件，不重刷卡、不改网络。

- [x] 大核 `ready result=0`，小核媒体/UI 启动及业务控制握手通过。
- [x] 先 `stop.sh` 后大核 `q`，正常返回 `msh`；IPC/OSD/camera/MAPI 清理均为 0。
- [x] 预览关闭且未绑定时 RGB 持续出帧；AI 约 30.3 fps，AI2D 约 1.9 ms、KPU 约 8 ms、后处理约 0.8 ms，含等待的循环约 33 ms。
- [x] 同时开启 AI、RTSP、录像；编码 H.264 1280x720 约 30 fps/4 Mbps，G.711A 单声道 8 kHz、320 字节/包。
- [x] Docker 真实 RTSP/TCP 客户端收到 30 个 H.264 VCL NAL、27 个 PCMA 包；不是模拟源。
- [x] 短录像完成 455 视频帧、120000 音频样本；约 8 MB，输出正式 `.mp4`。
- [x] 下载真实板端 MP4，FFmpeg 独立解码两轨通过：H.264/avc3 15.000 秒、alaw 15.001 秒。
- [x] 录像中关闭 RTSP 后录像继续；RTSP 再次开启，录像自动结束后 RTSP 仍运行。
- [x] 预览 VO 队列消费及开关：补齐帧结束中断后队列保持 0～1 帧，不再阻塞 RGB/编码。
- [ ] LCD 动态预览画面：后端已修复，仍需在实体屏幕上目视确认。
- [ ] 真实人脸的数量、框和关键点视觉检查：当前视野无脸，尚未确认。
- [ ] 3.1 寸屏幕中文可读性、四按钮/详情手指点按及触摸映射的人工验收。
- [ ] VLC 动态画面及麦克风实际声音的主观检查。
- [x] 预览、AI、RTSP、录像四功能后端并发：AI/编码约 30 fps，真实 RTSP 接收和 15 秒录像解码通过；不替代 LCD 视觉验收。

录像：`/sharefs/lawrec_records/clip-19700101T014954Z-WIQLZe.mp4`。
本机留存：`out/board-15s-avc3.mp4`。板端未校时，文件名日期不代表测试日期。
主要证据：`out/board-rtsp-client.log`、`out/board-avc3-short.log`、
`out/board-clip-decode.log`。这些本地输出不纳入源码 Git。

## 预览故障与修复

开启预览后，VO 输入队列达到 45 帧，RGB 取帧报 `0xa0158010`（NOTREADY），
编码初始化随后因无空闲 VB 块失败。不启动 Linux UI 也复现，因此尚不能将其
归因于 DRM/LVGL；真实 VO 扫描时间戳约 58.77 Hz，DSI 错误状态为 0，但这不
证明图层缓冲正常消费或显示。

最后提交图层配置、启用顺序及 VTTH 阈值行对照均未改善；无收益的阈值
改动已移除，继续保留固定显示参数。

当前预览 OFF 会解除 VI0→VO1 绑定，避免闲置 VO 消费者占满共用池，RGB AI 和
编码流保持运行。
仅重启小核不支持业务 IPC 自动重连；应先停止小核，再退出并重启大核，最后启动小核。
一次客户端接收在停止/重启交叠时超时，未声明所有客户端重连场景均通过。

最新定位：大核内核 `devmem2` 确认 VO IRQ133 优先级 1、使能位为 1、阈值 0；
普通用户态 PLIC 读取的全 `0xffffffff` 无效。启动不读取 stdin 的 12 秒预览探针，
保持 VTTH=1，仅将帧结束中断 `0x908403e0` bit20 从 0 改为 1 后，积压队列恢复
到 0，30 个编码缓冲恢复空闲；复原该位并自动停止，清理返回 0。

正式应用由 `VoSync` 在图层配置完成后启用帧结束中断；保留其他位和 VTTH，
停止摄像头/图层后恢复该位并解除映射，再释放 VB。未修改 SDK 或冻结适配。
最新启动读回 `frame-end=0x00100000 / VTTH=0x0010000a`，Linux VO 中断计数恢复增长。
已验证预览开关、四功能后端并发、30 个 H.264 VCL NAL/26 个 PCMA 包实际接收，
15 秒录像 455 帧/120000 音频样本，并独立解码通过。

最新录像：`/sharefs/lawrec_records/clip-19700101T061635Z-9IjNKg.mp4`；本机
`out/board-preview-sync.mp4`。证据：`out/board-preview-frame-irq.log`、
`out/board-preview-sync-buffers.log`、`out/board-preview-sync-rtsp.log`、
`out/board-preview-sync-record.log`、`out/board-preview-sync-decode.log`。
实体 LCD 是否正常显示动态画面、人脸框和关键点仍需人工确认，不能仅凭队列/状态标为通过。
