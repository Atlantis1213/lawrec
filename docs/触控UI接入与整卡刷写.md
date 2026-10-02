# 触控 UI 接入与整卡刷写

## 当前候选

用户反馈 LCKFB 新面板初始化序列的 DSI 图案测试正常，要求恢复 UI，并选择整卡
刷写。正常 UI/VO/摄像头组合尚待板测，不将图案测试等同于完整业务验收。

保留 27 MHz / 324 Mbps、pixclk_div=21、PHY n=3/m=52/voc=0x1f/hs_freq=0xb5、
K_DSI_4LAN 及通过测试的面板命令体。恢复正常大核入口、VO enable 和 VTTH=1；
关闭 DSI 测试图案，删除 60 秒提前退出和单独 timestamp 诊断调用。
正常 VO enable 自身包含时间戳操作。不改 Linux 触控驱动或软件滤波。

UI 默认启用触摸，480x800 原始轴缩放/边界限制，不增加镜像或旋转。
保留 GPIO53 作为备用输入。添加显示初始化失败日志，失败不报告预览后端就绪。

## 唯一推荐镜像

```text
\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-ui\20261001T155329Z\artifacts\sysimage-touch-ui.img
```

镜像大小 512 MiB，另有约 65 MiB 的 `.img.gz`，使用原始 `.img` 最直接。
SHA256：`796dcb199c2369c2647a7ecb5388311fe64476eab5c19f11c9fcba56ac9bb758`。

旧 `out/touch-ui/20261001T152743Z/` 候选未重新生成 ROMFS，不能作为本轮刷写输入。
新构建明确运行 SDK mkromfs，校验 RTT payload 中包含完整、字节一致的当前大核 ELF。
仅检查 root/bin 的 staging 文件或打印 build tag 不足以证明内嵌版本正确。

Docker 构建入口 `bash tools/build.sh touch-ui`；日志 `out/touch-ui-final-build.log`。
触摸解析针对性测试 `bash tools/test.sh touch` 已通过，见
`out/touch-ui-input-tests.log`。镜像内 rootfs UI/库/脚本、RTT/Linux 分区及内嵌 ELF
校验通过，产物 SHA256 校验通过。没有重新编译 Linux，保留原触摸适配。
构建仍有 SDK/AI 历史编译警告及 RTT RWX 链接警告，不声称无警告或正式发布。

## 整卡刷写

1. 导出需要保留的录像和配置，关闭板子电源，拔出 SD 卡接读卡器。
2. 在 Windows 的 balenaEtcher 选择上述 `.img` 和正确的 SD 卡设备，刷写并等待
   校验完成。整卡镜像会覆盖原分区和数据；不要选开发机系统盘，不必先手动格式化。
3. 安全弹出 SD 卡，装回板子，完整断电后重新上电。IP 可能重新由 DHCP 分配；
   若原地址不可达，在小核串口用 `ifconfig wlan0` 查询。

## 手动启动与验收

本轮保留大小核手动启动，刚开机黑屏不代表失败。无需再传测试 ELF。

大核串口 `msh />` 下，先确认 `list_process` 没有另一份业务程序，再执行：

```sh
/bin/fastboot_app.elf /bin/retinaface.kmodel /bin/mbface.kmodel
```

必须看到 `build touch-ui-lckfb-27m-4lane`、参数 `27000/324000`、
`ST7701 normal VO enable requested, VTTH=1`、connector result=0，
最终打印 `preview backend ready` 后再启动小核 UI。
不能继续运行 `/sharefs/lawrec-touch-*.elf`，那是旧诊断应用。

小核 Linux 终端执行：

```sh
LAWREC_TOUCH_ENABLE=1 LAWREC_TOUCH_TRACE=1 /app/lawrec/ui/ui &
```

正常日志路径：

```sh
tail -n 60 /tmp/lawrec.log
tail -n 20 /tmp/touch_trace.log
```

确认日志出现 `touch enabled map=480x800`。先松手观察 10 秒，再点击主页的
拍摄/设置/返回，检查界面响应、坐标方向、短点击和松手；进入拍摄后检查摄像头
预览。若异常，把大核启动日志、上面两份小核日志及可见现象发回。
不叠加触摸阈值或滤波，不同时测试音频/RTSP 并发，以先定位正常 UI 的触控边界。

验收后再安排开机自启动和其它媒体验收。本候选已经包含媒体/网络开发增量，
不在唯一 SSH 通道上操作 Apply Now 或换热点。未在本轮自动刷卡、重启或切网。

## 回退

已通过的面板图案诊断镜像仍保留在
`out/touch-panel-lckfb/20261001T094656Z/artifacts/sysimage-touch-panel-lckfb.img`。
回退需要重新刷该诊断镜像并完整断电，不是只换 ELF。它不运行正常 UI/媒体业务。
