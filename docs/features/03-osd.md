# OSD 检测框、关键点和计数

## 1. 入口和图层

实现位于 `big/osd.h`、`big/osd.cpp`。OSD 使用 SDK `K_VO_OSD2`，通过 `unsigned(layer)+3` 对应 OSD insert channel。Linux UI 使用独立的显示 overlay，不与这个图层共用 buffer。

OSD 分配一个 VB pool，包含两个 480×800 ARGB8888 block，用于双缓冲：每块 `480×800×4`，两块合计约 3000 KiB。每块映射为 CPU 可写的 `pixels_[2]`，`next_` 指向下一块。

## 2. 初始化

`Osd::start()` 创建 VB pool、获取两个 block、将物理地址写入 VO frame、映射 ARGB buffer、清零，然后设置 OSD 尺寸、格式、stride 和全局 alpha。任何一步失败都会调用 `stop()`；如果 buffer 已提交给硬件，停止函数会先尝试 disable OSD，再释放映射和 VB block。

## 3. 绘制

`Osd::show(faces)` 清空当前 buffer，对每个人脸调用 `portrait_face()` 将横屏检测坐标转换到 480×800 竖屏 OSD 坐标：

- 2 像素绿色人脸框。
- 每个关键点周围的橙色 5×5 小点。
- 顶部区域的两位数字检测计数。

像素写入函数对 x/y 做边界检查。当前最多允许 `max_faces`，超过上限返回错误。

## 4. 提交到 VO

绘制后先把 `submitted_` 置为 true，因为 insert 失败也可能已经把 buffer 交给硬件；随后调用 `kd_mpi_vo_chn_insert_frame()`。第一次成功插入时调用 `kd_mpi_vo_osd_enable()`，成功后切换双缓冲索引。

## 5. 清除和停止

`clear()` 调用 `kd_mpi_vo_osd_disable()`，只有成功才清除 `visible_`/`submitted_`。`stop()` 先 clear，再 unmap 两个 buffer、释放 block、销毁 pool；清理失败时保留资源状态，不强行释放仍可能被 VO 扫描的 block。

## 6. 与 AI、视频的关系

AI 关闭、预览关闭或视觉退出时清除 OSD，但不停止 CHN1 RGB capture 或 CHN2 encode capture。OSD 只显示在 LCD，检测框不会自动编码进 H.264/RTSP/MP4；要烧录到视频需要改变编码前数据路径，当前 Demo 没有这样做。
