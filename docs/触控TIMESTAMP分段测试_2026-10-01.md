# 触控 TIMESTAMP_ONLY 分段测试

## 本轮变化

用户已确认 VO_CONFIG_ONLY 组正常，加入最终 VO enable 才异常。本轮仅在该
配置后单独调用 `kd_vo_timestamp_enable()`，继续不执行 VO enable：

```c
connector_set_vo_init();
connector_set_vtth_intr(0, intr_line);
connector_set_vo_param(&attr);
// connector_set_vo_enable();
rt_kprintf("[touch-test] BEFORE timestamp only\n");
kd_vo_timestamp_enable();
rt_kprintf("[touch-test] TIMESTAMP_ONLY: no VO enable\n");
```

声明由当前 SDK 内核 `libvo.a` 提供；应用 ELF、时钟、PHY、DSI/面板配置
全部保持不变。临时修改在 ST7701 共用 helper 中，对其它 ST7701 屏型同样
生效，仅用于本板定位，不是正常业务版本。

Docker 反汇编确认 `kd_vo_enable()` 先将 `0x11` 写入显示寄存器地址，再调用
时间戳函数；本轮没有执行这一寄存器提交操作。时间戳函数读配置，bit0 已置位
就返回，否则调整相关位并写入。故两条日志证明调用前后已执行，不能证明
时间戳从关闭变为开启，本轮没有状态读回。`vo_init()` 内部短暂开启 VTTH
的路径也保持不变。若触控正常，仅说明本轮单独调用不足以触发，不能彻底
排除时间戳与 VO 配置生效后状态的组合影响。

## 用户反馈

用户反馈：单独时间戳调用后触控仍正常。下一轮保持本页内核固件不变，只把
应用 `dsi_test_mode` 设为 1，测试 DSI 内部图案，见
[DSI 图案测试](触控DSI_PATTERN分段测试_2026-10-01.md)。此结果不等于彻底
排除时间戳与 VO 输出启动后的组合影响，也不证明 MIPI 电磁干扰。

## 已构建归档产物

Docker 唯一入口：`bash tools/build.sh touch-timestamp`。
prepare_memory、mpp-kernel、big-core-opensbi、build-image 已完成，未重编
Linux 或改变触控适配；整卡打包重新生成 rootfs/DTB 包。内核两条新标记、
镜像 offset 10 MiB 的 RTT 字节和原 ELF 一致性校验通过。
SDK 默认 images 已更新为此定位版，正式发布目录未改动。

产物：`out/touch-timestamp/20261001T075532Z/artifacts/`。

| 文件 | 用途 |
| --- | --- |
| sysimage-touch-timestamp.img | 512 MiB 整卡烧录镜像 |
| sysimage-touch-timestamp.img.gz | 同一镜像压缩包 |
| rtt_system-touch-timestamp.bin | 独立大核固件 |
| lawrec-touch-clock-phy.elf | 原测试 ELF，名称保持不变 |
| SHA256SUMS | 文件校验和 |

相邻 `build.log` 保存日志，`backup/` 保存构建前镜像。

## 板端步骤

1. Windows 打开下面目录，用 balenaEtcher 烧录 `.img`。整卡烧录覆盖 SD 数据，
   所需录像或配置先导出。

   ```text
   \\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-timestamp\20261001T075532Z\artifacts
   ```

2. 完整断电再上电，不启动 UI，重新传入原测试 ELF（IP 用当前地址）：

   ```powershell
   scp "\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-timestamp\20261001T075532Z\artifacts\lawrec-touch-clock-phy.elf" root@192.168.123.74:/sharefs/lawrec-touch-clock-phy.elf
   ```

3. 小核先运行，观察松手时初始状态：

   ```sh
   chmod +x /sharefs/lawrec-touch-clock-phy.elf
   evtest /dev/input/event0
   ```

4. 大核串口运行：

   ```sh
   /sharefs/lawrec-touch-clock-phy.elf
   ```

   必须看到两条驱动新日志，应用旧 CLOCK_PHY_ONLY 名称不是阶段依据：

   ```text
   [touch-test] BEFORE timestamp only
   [touch-test] TIMESTAMP_ONLY: no VO enable
   ```

5. 先松手 10 秒，再点击中央和四角，再松手观察释放。记录是否无人触碰也
   持续上报、实际点击是否迟钝、DOWN/UP 是否完整，以及初始化返回值。
   本轮允许黑屏，测试 ELF 等待 60 秒退出，但不会恢复硬件；换阶段需冷启动。

## 判断

| 结果 | 下一步 |
| --- | --- |
| 触控正常 | 单独时间戳调用不足以触发；重点转向 VO 配置提交、输出启动及与时间戳的组合 |
| 再次迟钝或鬼触 | 本轮单独时间戳调用已足以触发；查它使用的硬件资源及大小核资源冲突 |
| 只有 BEFORE 或缺少新标记 | 先排查调用未返回、异常和烧录版本，不直接作触控结论 |

最终修复前需恢复完整显示链并验证触控，不将此定位版本用于 UI、摄像头、
推流或录像业务，也不因 VO enable 是触发边界就宣称已证实 MIPI 电磁干扰。
