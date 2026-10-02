# 触控 LCKFB 面板初始化序列测试

## 改动边界

本轮恢复 27 MHz / 324 Mbps、pixclk_div=21、PHY n=3/m=52/voc=0x1f/hs_freq=0xb5，
保留 K_DSI_4LAN。DSI 图案开启；VO 初始化和参数配置、VTTH 关闭请求、单独
timestamp 调用保持不变，最终 VO enable 仍跳过。

相对原四 lane TIMESTAMP_ONLY 基线，仅替换 SDK
`src/big/mpp/kernel/connector/src/st7701.c` 的 `st7701_480x800_init()` 函数体，
严格使用用户提供的 LCKFB 命令和延时，不改 DSI/VO 函数、触控或软件滤波。
应用源码与原 27 MHz 归档一致，复用相同 ELF。
用户引用的本地新版 `mipi_st7701.c` 未找到，未独立核对该官方源文件。

## 已完成 Docker 构建，未烧录

入口：`bash tools/build.sh touch-panel-lckfb`。
已执行 prepare_memory、mpp-kernel、big-core-opensbi、build-image；没有重编
Linux 或应用。镜像内 RTT 字节与独立大核包一致，面板函数反汇编已保存。

产物目录：`out/touch-panel-lckfb/20261001T094656Z/artifacts/`。

| 文件 | 用途 |
| --- | --- |
| sysimage-touch-panel-lckfb.img | 512 MiB 整卡诊断镜像 |
| sysimage-touch-panel-lckfb.img.gz | 同一镜像压缩包，先解压再烧录 |
| rtt_system-touch-panel-lckfb.bin | 独立大核固件，不是可执行应用 |
| lawrec-touch-lckfb-timing.elf | 字节不变的 27 MHz 测试应用 |
| st7701.c / main.cc / panel-init.disasm | 源码和内核函数快照 |
| SHA256SUMS | 产物校验和 |

整卡镜像 SHA256：
`667563917d3ea5a48b9c4c511b532df5fffc49324b06001e4610523366a4c0f7`。
大核包 SHA256：
`e0ccc7a9fd12b28280774f4b9430dd2efe945d7091513621658aaab9554e0830`。
应用 SHA256：
`7cf53bf24c4571150f56fbf5890dfe991c228c15f9f7864219eae43bc68bea3f`。

## 更新与测试

1. 导出需保留的录像和配置，再用 balenaEtcher 烧录本轮 `.img`。整卡烧录会覆盖
   SD 数据。Windows 目录为：

   ```text
   \\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-panel-lckfb\20261001T094656Z\artifacts
   ```

2. 完整断电上电，不启动 UI 或其它大核应用。重新上传同一个 27 MHz ELF：

   ```powershell
   scp "\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-panel-lckfb\20261001T094656Z\artifacts\lawrec-touch-lckfb-timing.elf" root@192.168.123.74:/sharefs/lawrec-touch-lckfb-timing.elf
   ```

3. 等传输结束，小核核对 SHA256 与上面的应用值一致，然后先运行 evtest：

   ```sh
   sha256sum /sharefs/lawrec-touch-lckfb-timing.elf
   chmod +x /sharefs/lawrec-touch-lckfb-timing.elf
   sync
   evtest /dev/input/event0
   ```

4. 大核串口执行，无需模型参数：

   ```sh
   /sharefs/lawrec-touch-lckfb-timing.elf
   ```

   参数应为 27000/324000，connector 返回 0，内核仍打印
   `[touch-test] TIMESTAMP_ONLY: no VO enable`。
   本轮为保持只替换函数体，没有新增面板版本日志；这条旧日志不能证明新面板
   序列已加载，必须烧录上面指定的新镜像。应用旧提示 `kernel unchanged` 也不
   表示本轮内核未改变。出现 `PHY_2LAN` 则不是本轮四 lane 固件。

5. 观察 30～60 秒：先不碰 10 秒，再点中央/四角并松手。记录图案是否正常、
   空闲上报次数、是否迟钝、按下/释放是否完整。程序退出不恢复硬件，下轮对照
   必须完整断电上电，不能同次上电连续运行两个版本。

## 判断与回退

| 结果 | 判断 |
| --- | --- |
| 图案正常，鬼触明显减少 | 支持面板初始化命令或延时参与影响；仍需正常 VO/UI 验证，不能完全排除触控驱动 |
| 图案正常，仍偶发或持续鬼触 | 保存事件和日志；本轮不能确认具体寄存器或干扰机制，不叠加触摸滤波 |
| 图案异常或不可见 | 输出对照不成立，先恢复原四 lane TIMESTAMP_ONLY 固件 |

回退镜像：`out/touch-timestamp/20261001T075532Z/artifacts/sysimage-touch-timestamp.img`，
配合同一个 27 MHz ELF，完整断电上电。单独换 ELF 不能回退面板命令。
本轮固件仅用于触控定位，不用于 UI、摄像头、RTSP、录像、回放的正式验收。
