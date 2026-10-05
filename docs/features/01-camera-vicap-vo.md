# 摄像头、VICAP、VB 与 VO 预览

## 1. 入口和职责

实现位于 `big/camera.h`、`big/camera.cpp`。`Camera` 是 VB、传感器采集、VICAP 通道和预览 VO layer 的唯一拥有者。

主要函数：

- `Camera::start()`：按 VB → 显示 → VICAP 的顺序初始化。
- `setup_buffers()`：配置公共 VB pool。
- `setup_display()`：设置 ST7701 应用侧 timing、VO layer 和 frame-end sync。
- `setup_capture()`：配置 GC2093、VICAP 三通道并启动 stream。
- `set_preview(bool)`：动态绑定/解绑 VI0 到 VO1。
- `snapshot_preview()`：只读读取 VO 当前地址，用于诊断，不从 VICAP dump 队列取帧。
- `stop()`：按仍在使用的资源状态逐步清理。

## 2. VB pool 配置

`setup_buffers()` 为预览、RGB AI、YUV 编码、VENC stream 和音频预留独立 pool：

| pool | 用途 | 单块大小 |
| --- | --- | --- |
| 0 | CHN0 800×480 YUV | `800×480×3/2` 对齐到 4 KiB |
| 1 | CHN1 1280×720 RGB planar | `1280×720×3` 对齐 |
| 2 | CHN2 1280×720 YUV | `1280×720×3/2` 对齐 |
| 3 | VENC stream buffer | `stream_block_bytes` |
| 4/5 | 音频和预留小块 | 2560/5120 字节 |

每个 pool 都设置 `VB_REMAP_MODE_NOCACHE`。函数累加 block size×count，保存为 `vb_kib_`，这个值是配置预算，不是运行时 allocator 的实时使用量。

## 3. ST7701 和 VO layer

`setup_display()` 通过 SDK 获取 `ST7701_V1_MIPI_2LAN_480X800` 连接器信息，然后覆盖已经验证的应用参数：

- 像素时钟 27 MHz，PHY clock 324 MHz。
- `pixclk_div=21`。
- PHY `n=3, m=52, voc=0x1f, hs_freq=0xb5`。
- 480×800 竖屏 timing。
- `dsi_test_mode=0`、`screen_test_mode=0`。

连接器 power/init 使用 SDK API；应用不修改内核面板序列，也不在 UI 进程重新调用 connector power/reset/init。

随后配置 `K_VO_LAYER1`：输入尺寸按 800×480 采集尺寸设置，使用 `PIXEL_FORMAT_YVU_PLANAR_420`，通过 `K_ROTATION_90` 显示到竖屏 LCD。layer 初始关闭，属性配置完成后调用 `kd_mpi_vo_enable()`。

VICAP 的 YUV semiplanar 格式和 VO layer 的 YVU planar 属性是两个不同 SDK 接口的约定，不能把 VO 格式枚举直接复制到 VICAP。

## 4. VO frame-end 同步

`VoSync` 通过 `/dev/mem` 映射 VO 基地址 `0x90840000`，只操作 frame-end interrupt register 的 bit 20：

1. 保存原 bit 状态。
2. 设置 frame-end bit 并用 memory fence 确认写入。
3. 在 `stop()` 中恢复原 bit，再解除映射。

它不修改 VTTH、MIPI timing 或 panel 参数。`preview_address()` 读取 layer1 的 Y/UV 地址，连续读取几次确保地址稳定，再返回物理地址；这条路径不会消费 SDK capture queue。

## 5. GC2093 和三通道 VICAP

`setup_capture()` 获取 `GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR` sensor info，设置 1920×1080 acquisition window 和 online mode，关闭 AF/AHDR/DNR3。

三个 channel 的统一配置如下：

| 通道 | 尺寸 | 格式 | 作用 |
| --- | --- | --- | --- |
| CHN0 | 800×480 | YUV semiplanar 420 | 预览，按需 VI0→VO1 bind |
| CHN1 | 1280×720 | RGB888 planar | `Detector::process()` dump |
| CHN2 | 1280×720 | YUV semiplanar 420 | Linux VENC0 bind |

三个通道都启用、使用 `capture_buffers=5`；只有 RGB CHN1 调用 `kd_mpi_vicap_set_dump_reserved(..., K_TRUE)`，因为 AI 通过 `kd_mpi_vicap_dump_frame()` 取它。

初始化 database parse mode、VICAP device 后调用 `kd_mpi_vicap_start_stream()`。摄像头 stream 始终保持运行，预览开关只控制 CHN0 的 VO 消费者。

## 6. 预览开关

打开预览：

1. `kd_mpi_vo_enable_video_layer(K_VO_LAYER1)`。
2. `kd_mpi_sys_bind(VI0, VO1)`。
3. 两步都成功后设置 `bound_` 和 `preview_`。

如果 bind 失败，立即关闭 layer，避免留下半启用状态。

关闭预览：先 `kd_mpi_sys_unbind(VI0, VO1)`，再关闭 VO layer，成功后更新状态。这样 CHN1 RGB 和 CHN2 YUV 仍可被 AI/编码使用，VO 不会在预览关闭时继续持有 CHN0 帧。

## 7. 关闭顺序和诊断

`Camera::stop()` 先停 stream；停失败时不释放 VB。之后解除绑定、deinit VICAP、关闭 VO layer、恢复 frame-end bit，最后退出 VB。

按 `v` 可以读取 `/proc/umap/vo`、`/proc/umap/vb` 和 VO 寄存器；按 `p` 可以通过 `VoSync::preview_address()` 做只读 NV12 snapshot。snapshot 可能发生 tearing，且是在 VO 旋转前的数据，不等价于 LCD 拍屏结果。
