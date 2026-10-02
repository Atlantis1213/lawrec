# 触控 LCKFB 显示参数 A/B 测试

本页记录原 27 MHz 时序测试。当前恢复同一个 27 MHz ELF、四 lane，改测面板
初始化序列，必须更新大核固件，见 [LCKFB 面板序列测试](触控LCKFB_PANEL分段测试_2026-10-01.md)。

## 前提与改动

用户确认：TIMESTAMP_ONLY 固件上，原参数 DSI 内部图案可见，但触控仍迟钝、
鬼触，未执行 VO enable。当前异常可在显示图案输出路径复现，不只由 VO
enable 调用触发；尚不能直接归因为 MIPI 电磁干扰。

本轮只修改应用 `sample_connector_init()`：获取 connector 信息成功后，
打开 connector 之前，在 LCKFB 编译分支覆盖以下用户提供的官方对齐候选。
`dsi_test_mode=1` 保持，`screen_test_mode` 不改。

| 参数 | 候选值 |
| --- | --- |
| pixclk_div | 21 |
| PHY n / m / voc / hs_freq | 3 / 52 / 0x1f / 0xb5 |
| pclk / phyclk（kHz） | 27000 / 324000 |
| H total / display / sync / back / front | 528 / 480 / 8 / 10 / 30 |
| V total / display / sync / back / front | 870 / 800 / 10 / 20 / 40 |

按 594 MHz 父时钟、除数编码 div+1 推算，`594000 / 22 = 27000 kHz`，
不直接传入 27561。对应 totals 的名义刷新率约 58.78 Hz；这些是配置/推算，
不是板端时钟读回或测量。多个参数作为一组对照，本轮不是逐个参数归因测试。

内核继续使用 TIMESTAMP_ONLY：保留 VO init、关闭 VTTH 请求、VO param 和
timestamp 调用，最终 VO enable 不执行。面板初始化命令、触控阈值、滤波、
坐标解析及 lane 配置均不改，保留已跑过的原参数 ELF 作为 A 组。

## 编译产物

只执行 Docker：`bash tools/build.sh big`。编译通过，新 ELF 标记已检查，
SDK `st7701.c` 和 `rtthread.elf` 与时间戳归档分别核对一致。不重新打包镜像。

B 组目录：`out/touch-lckfb-timing/20261001T083850Z/artifacts/`。
程序为 `lawrec-touch-lckfb-timing.elf`，附源码快照和 `SHA256SUMS`；日志在
相邻 `build.log`。SHA256：
`7cf53bf24c4571150f56fbf5890dfe991c228c15f9f7864219eae43bc68bea3f`。

A 组原程序仍在 `out/touch-dsi-pattern/20261001T080801Z/artifacts/` 和之前
已上传的 `/sharefs/lawrec-touch-dsi-pattern.elf`。`out/big/lawrec.elf` 已是 B 组
诊断程序，不是正常业务版。后续曾推进到 24.75 MHz，目前已恢复本页的
27 MHz ELF；复测应使用明确归档，并检查板端文件校验和。

用户后续反馈：27 MHz / 324 Mbps 有所改善，但仍偶发鬼触。未取得定量计数，
不宣称干扰幅度已经测量。下一轮整体替换应用参数为 24.75 MHz / 297 Mbps，
内核及触控不变，见 [24.75 MHz 时序测试](触控LCKFB_24750分段测试_2026-10-01.md)。
已确认板端保留 `/sharefs/lawrec-touch-lckfb-timing.elf`，可冷启动回退。

## 上传与测试，不重刷 SD

1. 板端联网后，Windows CMD / PowerShell 上传 B 组（IP 用当前地址）：

   ```powershell
   scp "\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-lckfb-timing\20261001T083850Z\artifacts\lawrec-touch-lckfb-timing.elf" root@192.168.123.74:/sharefs/lawrec-touch-lckfb-timing.elf
   ```

2. 等 SCP 成功结束，在小核检查校验和，匹配上面的值再运行，避免再次执行
   未传完的 ELF。然后 `sync` 并完整断电上电；不启动 UI 或其它大核应用。

   ```sh
   sha256sum /sharefs/lawrec-touch-lckfb-timing.elf
   chmod +x /sharefs/lawrec-touch-lckfb-timing.elf
   sync
   ```

3. 冷启动后，小核先运行 `evtest /dev/input/event0`，先观察无人触碰的基线。
   大核串口运行，无需模型参数：

   ```sh
   /sharefs/lawrec-touch-lckfb-timing.elf
   ```

4. 确认 connector 返回 0，日志中的分频、PHY、行场值与上表一致，并看到：

   ```text
   [touch-test] LCKFB_TIMING_AB: pclk=27000 phyclk=324000; TIMESTAMP_ONLY kernel unchanged
   [touch-test] DSI_PATTERN_ON; kernel must remain TIMESTAMP_ONLY
   [touch-test] TIMESTAMP_ONLY: no VO enable
   ```

5. 观察图案与触控 30～60 秒：先松手 10 秒，再点中央/四角，再松手观察释放。
   记录图案是否可见，触控是否迟钝、鬼触、能否正常 DOWN/UP。程序退出不恢复
   硬件，每次 A/B 对照都要断电重启，不能在同一次上电连续运行两组。

## 结果与回退

| 结果 | 判断 / 下一步 |
| --- | --- |
| 图案可见，触控恢复正常 | 支持这组显示参数改善异常；不能单独断言哪个参数或干扰机制，仍需恢复正常显示后的验证 |
| 图案可见，触控仍异常 | 下一轮对比并移植官方 LCKFB 面板完整初始化命令序列，不能只替换少数寄存器 |
| 图案不可见 | 本组输出验证不成立，不能据触控正常排除问题；冷启动后运行 A 组回退并确认原现象 |

回退只需冷启动，使用 `/sharefs/lawrec-touch-dsi-pattern.elf`，不换内核。
缺标记或初始化失败先排查加载、参数和固件版本，不作触控归因。

## 后续 lane 修正，暂不实施

本地 `st7701_set_phy_freq()` 将 `mipi_phy_attr.phy_lan_num` 写死为
`K_DSI_4LAN`，而该屏型为两 lane。应用 DSI lane 与 PHY lane 是不同配置入口，
只改应用无法消除该硬编码。本轮暂时保留，避免混入第二类改动。

完成本轮后再独立修改该屏型的 PHY lane 为 `K_DSI_2LAN`，核对其它 ST7701
屏型影响，并用 Docker 更新大核固件；只换 ELF 不生效。不能现在就宣称
该修正已完成或已证实是根因。
