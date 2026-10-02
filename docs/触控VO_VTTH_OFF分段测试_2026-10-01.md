# 触控 VO / VTTH-off 分段测试

## 本轮目的

用户反馈 CLOCK/PHY、DSI/面板组正常，加入 VO 后出现异常。本轮恢复完整
`st7701_init()`，只把 `st7701_vo_resolution_init()` 中的
`connector_set_vtth_intr(1, intr_line)` 改为 `0`。顺序保持：

```c
connector_set_vo_init();
connector_set_vtth_intr(0, intr_line);
connector_set_vo_param(&attr);
connector_set_vo_enable();
```

不改像素分频、PHY、显示时序或面板命令；DSI 初始化仅调用一次。此改动在
ST7701 共用 VO helper 中，对该驱动其它屏型也生效，仅用于当前 480x800 板测试。
应用沿用通过 CLOCK/PHY 测试的同一归档 ELF，不重新编译。

封装 `connector_set_vtth_intr()` 返回 void，直接调用 `kd_vo_set_vtth_intr()`，
本轮未读回中断掩码/状态。因此新标记证明调用了关闭请求，不证明硬件已经
关闭；VO enable 封装同样没有结果读回。关闭 VTTH 可能影响显示同步，不能
作为最终修复，也不要启动正常 UI、摄像头、RTSP 或录像来混合测试。

## 用户反馈与阶段限制

用户板测反馈：恢复 VO 后，即使请求关闭 VTTH，evtest 仍迟钝、鬼触。
本地 SDK `kernel/lib/libvo.a` 的 Docker 反汇编也确认：`vo_init()` 在
`+0x116` 设置 `a1=10`、`+0x118` 设置 `a0=1`，随后调用
`kd_vo_set_vtth_intr()`。因此外层关闭请求不能证明初始化全程没有中断影响，
不能据此排除 VTTH。下一轮只跳过最终 enable，见
[VO 配置分段测试](触控VO_CONFIG分段测试_2026-10-01.md)。

## 已构建的归档固件

Docker 唯一入口：

```sh
bash tools/build.sh touch-vo-vtth-off
```

已完成 prepare_memory、mpp-kernel、big-core-opensbi、build-image；未重新编译
Linux 或修改其触控适配。整卡打包会重新生成 rootfs/DTB 包，因此保留上一轮
镜像对照。内核新标记、镜像 offset 10 MiB 的 RTT 字节及 ELF 一致性校验通过。
SDK 默认 images 曾更新为本轮，现在已推进到 VO_CONFIG_ONLY；复测本轮须
使用下面的归档镜像。正式发布目录未改动。

产物目录：`out/touch-vo-vtth-off/20261001T072424Z/artifacts/`。

| 文件 | 用途 |
| --- | --- |
| sysimage-touch-vo-vtth-off.img | 整卡烧录，512 MiB |
| sysimage-touch-vo-vtth-off.img.gz | 同一镜像压缩包 |
| rtt_system-touch-vo-vtth-off.bin | 独立大核固件 |
| lawrec-touch-clock-phy.elf | 原测试 ELF，名称保持不变 |
| SHA256SUMS | 产物校验和 |

RTT SHA256：`b441d979d920c0c1412517b38358076ad3c853f9c6b0499b3b1e4a899a8d0d84`。
ELF SHA256：`a09fcbfedae79330ce54cb38a52b1bb2db73c506c9800373b403b32cd0ba6277`。
构建日志在相邻 `build.log`；构建前镜像备份在相邻 `backup/`。

## 板端操作

1. Windows 打开下列目录，用 balenaEtcher 烧录 `.img`。整卡重刷会覆盖 SD 数据，
   有需要保留的录像或配置先导出。

   ```text
   \\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-vo-vtth-off\20261001T072424Z\artifacts
   ```

2. 完整断电再上电，保持 UI 关闭。将同一个 ELF 重新传入 sharefs；下面 IP
   以板端当前地址为准。

   ```powershell
   scp "\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-vo-vtth-off\20261001T072424Z\artifacts\lawrec-touch-clock-phy.elf" root@192.168.123.74:/sharefs/lawrec-touch-clock-phy.elf
   ```

3. 小核 SSH 先执行，先观察未启动测试程序的空闲状态：

   ```sh
   chmod +x /sharefs/lawrec-touch-clock-phy.elf
   evtest /dev/input/event0
   ```

4. 大核串口执行：

   ```sh
   /sharefs/lawrec-touch-clock-phy.elf
   ```

   必须看到驱动标记，旧应用 `CLOCK_PHY_ONLY candidate` 提示不代表当前阶段：

   ```text
   [touch-test] VO_ENABLED_VTTH_OFF: VO enabled, VTTH requested off
   ```

5. 先完全松手 10 秒，再点击中央和四角，最后松手观察释放。另一个 SSH 终端
   在测试程序运行期间记录 Linux 中断变化：

   ```sh
   cat /proc/interrupts > /tmp/irq-before.txt
   sleep 10
   cat /proc/interrupts > /tmp/irq-after.txt
   cat /tmp/irq-before.txt
   cat /tmp/irq-after.txt
   ```

Linux `/proc/interrupts` 只能观察小核管理的中断，不能据此证明大核 VTTH 已
关闭。记录是否无人触碰也持续上报、点击是否迟钝、DOWN/UP 是否完整，以及
大核初始化返回值。程序等待 60 秒后退出，但不会恢复硬件，下一轮必须冷启动。

## 结果判断

| 结果 | 下一步 |
| --- | --- |
| 恢复 VO 后正常 | 强烈提示 VTTH 中断路径参与触发；检查频率、清除方式和大小核处理，不直接宣布修复 |
| 仍迟钝或鬼触 | 先核对 VTTH 实际掩码/状态及是否被其它路径重新使能；确认关闭后再查 VO 扫描、持续 DSI 传输和显示时序 |
| 缺少新标记或初始化失败 | 先核对烧录固件和初始化过程，不能判断该分段结果 |

VO 开始扫描后 DSI 进入持续传输，前两组正常仍不能排除 MIPI/时钟相关干扰。
