# Demo 部署、启动与短验收

当前状态：应用已完成离线构建，板端功能仍待验收。未生成 SD 卡镜像，未修改
SDK、内核或网络设置，也未连接开发板。
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

基础功能各验证一次即可，不需要长测或反复压力测试。

1. 点击 **实时预览**，确认摄像头画面会随物体移动变化，而非仅显示“已开启”。再点击 **人脸检测**，确认人脸框、数量、关键点以及非零的实际耗时和帧率。
2. 保持预览和检测开启，点击 **网络推流** 和 **视频录像**。在电脑执行以下命令，将 `<board-ip>` 替换为开发板 IP：

```sh
vlc --rtsp-tcp rtsp://<board-ip>:8554/lawrec
```

3. 确认 VLC 有画面和麦克风声音。录像进行中关闭推流，确认录像继续；重新开启推流，再关闭录像，确认 VLC 继续播放。新录像按源时间戳（PTS）计时约 15 秒后自动停止，无需连续测试 10 次。
4. 将一段已完成的 `/sharefs/lawrec_records/clip-*.mp4` 复制到电脑，确认画面、声音正常，时长约 10～20 秒。`.mp4.part` 表示未完成或失败，不能算录像成功。保存一次状态输出和运行日志。
5. 在小核执行 `stop.sh`，确认退出后在大核输入 `q`。将实际结果填写到 `docs/acceptance.md`；未通过的项目保留为待验收，并记录首次错误及日志。

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

已上传的临时修正版可在旧 vision 确认退出后从大核串口执行：

```sh
/sharefs/lawrec-demo/bin/vision-connector-fix.elf /sharefs/lawrec-demo/models/retinaface.kmodel
```

该文件单独上传并校验，旧应用包和原 `vision.elf` 未覆盖。
后续从修复提交重新打包时，修复会包含在标准路径 `bin/vision.elf` 中。

界面帧率是实际回调速率，不保证等于屏幕刷新率或客户端播放帧率。
CPU/RSS 仅统计 `media_service`；`democtl` 中的 `UINT32_MAX` 表示数据不可用。
VB 数值是配置的缓冲池预算，不是系统实际总占用。
当前 DSI、PHY 和触控适配已固定，排查摄像头或编解码错误时不要重新调整显示时序。

旧工程的 CHN0 NOTREADY 和 vblank 日志不能作为新 Demo 的运行证据。
应优先查看本次 vision、媒体服务日志，以及屏幕是否真正显示动态画面。
离线测试不能证明板端传感器出帧、OSD 混合、KPU 推理、麦克风及音视频时间戳、
VLC 播放已正常；这些仍需上述短验收确认。
