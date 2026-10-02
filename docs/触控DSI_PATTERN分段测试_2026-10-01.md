# 触控 DSI 内部测试图案

## 单一变量

用户反馈 TIMESTAMP_ONLY 组正常。保持该大核固件不变，只在应用
`sample_connector_init()` 中、唯一一次 `kd_mpi_connector_init()` 调用前添加：

```c
connector_info.dsi_test_mode = 1;
printf("[touch-test] DSI_PATTERN_ON; kernel must remain TIMESTAMP_ONLY\n");
fflush(stdout);
```

同时纠正应用旧的固件要求提示为 TIMESTAMP_ONLY；其它参数和业务不改。
`screen_test_mode` 不设置。SDK 已有 `dsi_test_mode == 1` 分支，调用
`connector_set_dsi_test_mode()`，最终调用库的 `dwc_dst_set_test_mode()`。
VO enable 仍跳过，时间戳单独调用保留；测试应用只初始化 connector、等待
60 秒后退出，不启动摄像头、IPC、UI 或正常业务。

本地库 Docker 反汇编确认 `dwc_dsi_enable()` 只保存输入参数，随后无条件
初始化并写入使能值 1，未使用该参数作开关。本轮不调用 `dsi_enable(0)`
作为关闭对照，不修改内核，也不追加其它寄存器操作。

## 构建结果

仅执行 Docker 入口 `bash tools/build.sh big`，编译通过，应用新标记已检查。
SDK `st7701.c` 及 `rtthread.elf` 与 TIMESTAMP_ONLY 归档分别核对一致；没有
执行 firmware/image 构建，镜像保持上一轮版本。已部署并校验文件，图案输出
和触控结果尚未实板验证。

新产物：`out/touch-dsi-pattern/20261001T080801Z/artifacts/`。
其中 `lawrec-touch-dsi-pattern.elf` 是本轮程序，`main.cc` 是源码快照，
`SHA256SUMS` 是校验和；相邻 `build.log` 保存 Docker 日志。

新 ELF SHA256：`9f75df11c4406a557b695ddebb74d7a9e51ca87c7195b4c0fb44f14f618c29eb`。
`out/big/lawrec.elf` 也已更新为本轮测试应用，不是正常业务程序。

## 首次加载失败已排查

首次串口出现 `lwp load faild, -1`、`command not found`，此时未执行应用。
通过 SSH 核对：板端文件仅 9,240,576 字节，本地应为 11,812,152 字节，
SHA256 不一致；sharefs 挂载正常且剩余约 140 MiB，传输进程已退出。
因此已确认文件不完整，而不是该日志证明了 DSI 初始化失败；具体传输
中断原因未知。RT-Smart ELF 加载器读取越界/短读也会返回 -1。

已重新上传到 `.elf.incoming`，SHA256 匹配后替换正式文件并执行 `sync`，
最终 `/sharefs/lawrec-touch-dsi-pattern.elf` 再次校验与上述 SHA256 一致。
没有改代码或固件。后续上传须等待 SCP 成功结束，再检查大小及校验和，
不要在写入未完成时运行 ELF。小核可核对：

```sh
ls -l /sharefs/lawrec-touch-dsi-pattern.elf
sha256sum /sharefs/lawrec-touch-dsi-pattern.elf
```

## 上传和运行，不重刷 SD

前提：板端保留已测试的 TIMESTAMP_ONLY 固件。如果不是该固件，要先恢复
上一轮版本；单独换应用不能更改内核。原时间戳测试 ELF 保留，勿覆盖。

1. Windows CMD / PowerShell 上传新 ELF，IP 按当前地址修改：

   ```powershell
   scp "\\wsl$\Ubuntu\home\atlantis\lawrec\out\touch-dsi-pattern\20261001T080801Z\artifacts\lawrec-touch-dsi-pattern.elf" root@192.168.123.74:/sharefs/lawrec-touch-dsi-pattern.elf
   ```

2. 完整断电再上电，不启动 UI 或其它大核应用。小核先观察空闲触控：

   ```sh
   chmod +x /sharefs/lawrec-touch-dsi-pattern.elf
   evtest /dev/input/event0
   ```

3. 大核串口运行，无需模型参数：

   ```sh
   /sharefs/lawrec-touch-dsi-pattern.elf
   ```

   必须看到应用标记和原内核标记，以及 connector 返回 0：

   ```text
   [touch-test] DSI_PATTERN_ON; kernel must remain TIMESTAMP_ONLY
   [touch-test] BEFORE timestamp only
   [touch-test] TIMESTAMP_ONLY: no VO enable
   ```

   开头保留的 `CLOCK_PHY_ONLY candidate` 是旧应用名称，不代表当前阶段。

4. 先松手 10 秒，再点击中央和四角，最后松手观察释放。同时观察屏幕是否
   实际显示测试图案。请记录两个结果：有没有图案、evtest 是否迟钝或鬼触。
   程序退出不恢复硬件，下一次对照仍须冷启动。

## 判断

用户已反馈：图案可见、触控仍迟钝和鬼触，未执行 VO enable。异常可以在
DSI 内部图案路径复现，应优先比较显示传输/扫描/时序，但仍不是 MIPI 电磁
干扰的直接证据。下一轮只覆盖应用中的候选显示参数，内核不改，见
[LCKFB 时序 A/B 测试](触控LCKFB_TIMING分段测试_2026-10-01.md)。

| 屏幕 / 触控结果 | 意义 |
| --- | --- |
| 出现测试图案，同时鬼触 | 不执行 VO enable 也能通过图案输出触发；重点查显示传输、面板扫描与触控耦合及显示时序，尚非电磁干扰的直接证据 |
| 出现图案，触控正常 | 继续重点查 VO 配置提交后的状态；图案与正常输出波形不同，仍不能完全排除显示链路影响 |
| 没有图案，触控正常 | 不能作为排除依据，可能图案并未实际输出 |
| 没有图案但触控异常，或初始化失败 | 保留完整日志，先确认实际执行和输出状态，不直接按上述两组归因 |

DSI 初始化先启用图案、随后调用 VO init/参数，后续步骤是否影响图案输出
必须通过屏幕观察确认；打印标记不能替代实际图案出现。
