# Demo 部署、启动与短验收

当前状态：已在板端验证两核启动/正常退出、AI 推理、音视频编码、RTSP 数据接收和
15 秒 MP4 录像及独立解码。预览的 VO 队列积压已修复，预览/AI/RTSP/录像并发
短测通过；**LCD 动态画面及人脸框/关键点仍需一次目视确认。**
未生成 SD 卡镜像，未修改 SDK、内核、触控或网络设置；本次更新仅包含应用。
请使用此前触控验证通过的 **27 MHz / 4 lane / ST7701 / VTTH=1** 固件，
不要使用分段触控测试内核。应用包不能保证兼容其他固件。

## 准备应用包

如需重新打包，在开发机的项目目录执行；工作区应无未提交修改：

```sh
bash tools/build.sh package
```

该命令在新的发布目录重新构建业务程序和 LVGL，检查 ELF 与固定适配代码，
从对应 SDK 收集完整运行库依赖，校验并复制模型，生成文件校验值。
产物 `out/releases/<commit>-<UTC>/lawrec-demo-<commit>.tar.gz` 是
**应用压缩包，不是烧录镜像，不需要重刷 SD 卡**。
`meta/` 保存构建日志、编译器版本、SDK 路径、Docker 镜像 ID、源码提交及运行库来源。

板端验收前，先停止旧媒体服务和 UI，并在大核串口输入 `q` 正常退出旧应用，
再复制、解压新包。不要同时启动旧 lawrec 自启服务或其他占用摄像头的程序。
建议解压到新的空目录 `/sharefs/lawrec-demo`，不要覆盖正在运行的程序。
在小核 Linux 终端执行校验：

```sh
cd /sharefs/lawrec-demo
sha256sum -c SHA256SUMS
```

录像目录所在分区至少预留 **32 MiB** 空间。RTSP 使用 IPv4、端口 `8554`，
控制 socket 为 `/var/run/lawrec-demo.sock`，沿用板端现有网络。
应用包不替换 `/lib`、`/usr/lib`、SD 卡启动文件或系统自启脚本。
`run.sh` 使用包内配套的 SDK 加载器和独立库路径；板端内核与设备仍须匹配。

## 启动与停止

**第一步：在大核 RT-Smart 串口启动 vision。** 只运行这一个摄像头管理程序：

```sh
/sharefs/lawrec-demo/bin/vision.elf /sharefs/lawrec-demo/models/retinaface.kmodel
```

**第二步：等大核打印 `[vision] ready result=0`，再在小核 Linux 终端执行：**

```sh
cd /sharefs/lawrec-demo
sh start.sh
sh run.sh democtl status
```

无需传入 `mbface` 模型。实时预览、人脸检测、网络推流、视频录像默认均关闭。
`start.sh` 只启动 `media_service` 和 `demo_ui`，日志位于 `/tmp/lawrec-demo`，
进程 PID 和启动时间记录位于 `/var/run/lawrec-demo`。
脚本拒绝重复启动，服务还通过 socket 锁防止多实例。
IPC 握手成功不代表屏幕已有画面；脚本不会通过未经验证的远程 shell 启动大核。

**停止顺序：先在小核停止 UI 和媒体服务：**

```sh
sh stop.sh
```

确认小核媒体服务真正退出后，再在大核 vision 串口输入 **`q`**。
不要使用 `Ctrl+C` 或 `killall`。
`stop.sh` 仅停止启动时登记且 PID、启动时间、命令身份匹配的进程，先停 UI、
后停媒体服务，每个进程最多等待 5 秒；拒绝未知或被复用的 PID，
不会用 `SIGKILL` 强杀 MP4 写入进程。
如果停止超时，保持 vision 运行，查看日志后重试 `stop.sh`。
不要在媒体回调可能仍运行时释放摄像头或 VB 资源；当前没有自动恢复机制。

## 一次短验收

基础功能各验证一次即可，不需要长测或反复压力测试。已完成的自动检查见
`docs/acceptance.md`，不必重复 15 秒录像。剩余人工检查如下：

1. 点击 **实时预览**，在镜头前移动物体，确认屏幕画面动态变化；再点击 **人脸检测**，在镜头前放置人脸，确认数量、框和关键点。若这些功能已经开启，不必重复点击开关。
2. 点击 **网络推流**。在电脑执行以下命令，将 `<board-ip>` 替换为开发板 IP：

```sh
vlc --rtsp-tcp rtsp://<board-ip>:8554/lawrec
```

3. 确认 VLC 有动态画面和麦克风声音。自动 RTSP 客户端已经收到真实 H.264/G.711A 数据，但不替代这一项主观检查。
4. 如需演示录像，点击 **视频录像**，按源时间戳（PTS）计时约 15 秒后自动停止。`.mp4.part` 表示未完成或失败，不能算成功；最新四功能并发录像 `clip-19700101T061635Z-9IjNKg.mp4` 可直接复制到电脑播放。文件名日期取自板端系统时间，不影响媒体时长。
5. 在小核执行 `stop.sh`，确认退出后在大核输入 `q`。**重新启动时两核都要重启**，当前业务 IPC 不支持仅重启小核后的自动重连。

现在可以同时开启四个功能。旧版预览曾造成 VO 队列积压和共用 VB 池耗尽，
随后 RGB 取帧和编码失败；修复版补齐帧结束中断后，队列保持 0～1 帧。
控制返回成功仍不能替代 LCD 动态画面的目视确认。

## 可选命令行控制

若按钮无响应，可用以下命令区分 UI 问题与后端问题：

```sh
sh run.sh democtl preview on
sh run.sh democtl ai on
sh run.sh democtl rtsp on
sh run.sh democtl record on
sh run.sh democtl status
sh run.sh democtl record off
sh run.sh democtl rtsp off
```

## 指标与排错说明

若旧包打印 `connector init result=0`，随后打印 `connector close result=-4096`
并以 `ready result=-4096` 退出，这是应用误读 SDK 关闭接口返回值的问题，
不是触控或显示时序失败。SDK 的 `kd_mpi_connector_close()` 实现缺少返回值；
修正版使用 `close(fd)` 并检查真实错误。只有 `ready result=0` 才可启动小核。

修复包含在标准路径 `bin/vision.elf`。旧 `vision-connector-fix.elf` 仅修复关闭
接口，不含后续 IPC、退出和预览绑定修复；不要再使用它启动本次版本。
更新应用时必须先完成上述停止顺序，再一起更新 `vision.elf`、`media_service`
及包内配套库，并校验新的 `SHA256SUMS`。

SDK IPC 的 TRY_CONNECT 返回 1 表示对端尚未接入，不是连接失败；应用修复了
此返回值处理。正常退出还会取消 MAPI 等待，避免 `q` 卡在等待小核重连。
SDK 编码器在 IDR 帧会改变 SPS/PPS 的 ID，录像采用 `avc3` 并保留随帧参数集，
不再将参数集字节变化误判为致命错误。

预览修复版启动应出现 `[vision-sync] frame-end IRQ=0x00100000 VTTH=0x0010000a`。
固定 SDK 的 connector 仅启用 VTTH，未启用驱动消费帧队列所需的帧结束中断。
应用仅修改 VO `0x3e0` 的 bit20，退出时恢复原值；没有修改时钟、PHY、VTTH 或面板序列。
这一适配绑定当前冻结 SDK，不能把寄存器写法直接用于其他 SoC/固件。
大核串口输入 `v` 可读取 VO/VB 状态；正常预览不会将全部视频缓冲引用堆积在 VO。

若预览仍黑屏，大核串口输入 `p` 可保存 `/sharefs/lawrec-preview.nv12`，
它是 VO 视频层地址对应的 800×480 NV12 只读采样，不使用 SDK VO dump 接口，
不会申请新的摄像头通道。采样可能撕裂，仅用于确认输入画面，不代表 LCD 实际显示。
在开发机下载后可转换为竖屏图片：

```sh
scp -O root@192.168.123.74:/sharefs/lawrec-preview.nv12 /tmp/lawrec-preview.nv12
ffmpeg -f rawvideo -pixel_format nv12 -video_size 800x480 -i /tmp/lawrec-preview.nv12 \
  -vf transpose=1 -frames:v 1 /tmp/lawrec-preview.png
```

当前冻结 SDK 的 `kd_mpi_vo_chn_dump_frame`/release 在本次现场诊断中导致
RGB 取帧和编码停止、退出时 VB 引用残留，最终版本没有调用这组接口。
只读采样与 SDK dump 完全不同；排错时不要重新加入该 dump 调用。

界面帧率是实际回调速率，不保证等于屏幕刷新率或客户端播放帧率。
CPU/RSS 仅统计 `media_service`；`democtl` 中的 `UINT32_MAX` 表示数据不可用。
VB 数值是配置的缓冲池预算，不是系统实际总占用。
当前 DSI、PHY 和触控适配已固定，排查摄像头或编解码错误时不要重新调整显示时序。

旧工程的 CHN0 NOTREADY 和 vblank 日志不能作为新 Demo 的运行证据。
应优先查看本次 vision、媒体服务日志，以及屏幕是否真正显示动态画面。
真实 RGB/编码出帧、KPU 推理、RTP 接收和 MP4 解码已有板端证据；LCD 预览、
人脸 OSD、手指点按及 VLC 画面/麦克风声音主观检查仍未通过完整验收。
