# 构建、测试和部署

## 1. 构建规则

所有编译通过 K230 SDK Docker 执行，不能在宿主机直接调用交叉编译器。`K230_SDK_ROOT` 指向外部 SDK；SDK、工具链、模型、binary 和 `out/` 不入 Git。

```sh
export K230_SDK_ROOT=/home/atlantis/k230_sdk
bash tools/build.sh all
bash tools/build.sh test
bash tools/build.sh rtsp
bash tools/build.sh media
bash tools/build.sh ui
bash tools/build.sh verify
bash tools/build.sh bundle-check
bash tools/build.sh package
```

`all` 构建 vision 和 Linux 应用；`test` 构建/运行纯逻辑测试；`rtsp` 编译 live555/RTSP 测试；`media` 编译 libmov/FFmpeg/x264 媒体测试；`ui` 使用 SDK LVGL 和实际页面检查；`verify` 检查 ELF 和动态依赖；`bundle-check` 检查应用包。

## 2. 编译目标

`big/CMakeLists.txt` 只列 camera、control、detector、main、mapi_wait、osd、vo_sync 和必要 common 文件，SDK nncase/MPP/系统库从 sysroot 链接。

`little/CMakeLists.txt` 生成 `media_service`、`demo_ui`、`democtl`，使用 explicit source lists，不把旧 lawrec 的 WiFi、网络恢复、播放和文件管理模块通过 wildcard 编入。

## 3. 模型和硬件基线

vision 启动参数是一个 RetinaFace kmodel 路径：

```sh
vision.elf /bin/retinaface.kmodel
```

模型不提交 Git。部署前核对 README 记录的 SHA-256。显示适配使用已验证的 27 MHz、divider 21、PHY `n=3/m=52/voc=0x1f/hs=0xb5`、4 lanes、ST7701 sequence 和 VTTH=1；不重新安装触控或 panel driver。

## 4. 测试分类

纯逻辑测试覆盖 faces、Annex-B/SPS/PPS/IDR、PTS、FrameQueue 双消费者、protocol、socket、IPC driver ABI、VoSync 和 OSD 状态。真实媒体库测试覆盖 live555 SDP/RTP、libmov 封装、FFmpeg 解码、UI layout；媒体输入多为模拟 codec。

这些测试不能证明 sensor 出帧、LCD 画面、触控手感、KPU 实际耗时、硬件 G.711A、VICAP 三路并行、VLC 主观播放或 SD 卡长期可靠性。

## 5. 部署和启停

部署包应包含 vision ELF、media_service、demo_ui、democtl、匹配动态库、model 校验说明、启停脚本和 RTSP URL。

启动顺序：`vision.elf → media_service → demo_ui`。停止顺序：`demo_ui → media_service（先结束 RTSP/MP4）→ vision.elf（q）`。脚本只管理本次启动的 PID，不使用宽泛 `killall`，不自动修改网络。

## 6. 最终验收

上板后依次检查预览、AI/OSD、RTSP 视频和音频、15 秒 MP4、四项功能并行、分别停止 RTSP/录像和程序退出清理。保存串口日志、`democtl status`、VLC/ffprobe 输出和录制文件，在 `docs/acceptance.md`、`docs/devlog.md` 记录 commit、SDK、参数、结果和未通过项目。
