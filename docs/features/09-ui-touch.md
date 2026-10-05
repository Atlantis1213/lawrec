# LVGL 页面与触控输入

## 1. UI 组成

入口是 `little/ui/main.cpp`，页面为 `little/ui/page.cpp`，socket client 为 `common/socket.cpp` 和 `little/ui/ctl.cpp`，触控适配位于 `compat/lvgl_port/k230/lv_port_indev.c`。

UI 不调用 connector power/reset/init，也不申请摄像头、VB、VENC 或 AENC；屏幕和触控使用已验证的 LVGL port。

## 2. UI 主循环

`main()` 依次注册信号、`lv_init()`、显示 port、输入 port、`Page::create()`，创建 200 ms refresh timer，再启动 communications worker。主线程每约 5 ms 调用 `lv_timer_handler()`。

通信线程每轮取出 pending command，通过 socket exchange 查询/设置状态，更新 `snapshot`、transport error 和 operation error，再等待约 500 ms。LVGL 主线程不等待 socket。

## 3. 页面布局

页面固定为 480×800 竖屏：顶部标题/反馈和“详情”按钮；中间透明 preview band；底部四个 216×104 touch target：实时预览、人脸检测、网络推流、视频录像；底部显示 face count、AI latency、video FPS、bitrate。

详情 modal 显示 AI2D/KPU/post/total、AI/video FPS、码率、队列、media CPU、RSS、VB budget 和错误码。

## 4. 点击处理

LVGL callback 只判断 modal、active/busy/连接状态，并把 command/value 写入 pending slot。真正 socket 调用在通信线程执行，返回后清理 busy 并记录 operation error。重复点击或 busy 状态下的按钮被禁用；RTSP/录像 stop 在后台断开时仍可发送，便于释放本地媒体 worker。

## 5. 触控输入适配

`lv_port_indev.c` 扫描 evdev，识别触摸设备，查询 ABS 范围并缩放到 480×800；支持单点 ABS 和 MT slot/tracking id，将 press/release 组合成 LVGL 状态。事件 frame 不完整或同步丢失时等待完整 frame；设备读失败时关闭 fd，并按 1 秒间隔重试。

当前触控硬件适配已经完成；本项目只使用它，不改变 CST128 初始化、MIPI clock、PHY 或 reset 时序。

## 6. 命令行控制

`little/ui/ctl.cpp` 编译为 `democtl`，可绕过 LVGL 验证同一协议：

```sh
democtl status
democtl preview on
democtl preview off
democtl ai on
democtl ai off
democtl rtsp on
democtl rtsp off
democtl record on
democtl record off
```

它只操作 Unix socket，不直接操作硬件。

## 7. UI 错误边界

连接失败显示“后台未连接”；操作失败显示按钮和 errno；vision 异步错误通过 status.last_error 保留，不会在下一次成功查询时被清零。缺失 metrics 显示 `--`，不用 0 伪装测量结果。
