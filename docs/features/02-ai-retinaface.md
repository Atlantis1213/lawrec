# MobileRetinaFace、AI2D 与 KPU

## 1. 入口

实现位于 `big/detector.h`、`big/detector.cpp`，检测主循环位于 `big/main.cpp`。

- `Detector::load(path)` 只执行一次模型加载和 AI2D schedule 创建。
- `Detector::process(faces, timing)` 获取一帧 CHN1 RGB，执行预处理、KPU 和后处理。
- `AiTiming` 返回 AI2D、KPU、postprocess、total 四项微秒耗时。

实现使用 SDK nncase runtime 和 AI2D，不依赖 OpenCV。

## 2. 模型契约检查

`load()` 打开模型文件后验证固定模型接口：

- 1 个输入，9 个输出。
- 输入 `1×3×320×320`，`uint8`。
- 输出为 9 个 float32 head。
- 三组输出空间尺寸为 40×40、20×20、10×10。
- 三组输出 channel 分别为 8、4、20。

输入 tensor 从 `hrt::create(..., hrt::pool_shared)` 创建，并绑定到 interpreter input。模型不在每帧重复加载。

## 3. AI2D letterbox

摄像头输入是 1280×720 RGB planar，模型输入是 320×320 NCHW。保持宽高比时，1280×720 缩放到 320×180，上下各补 70 像素。

`ai2d_builder` 配置：输入/输出格式都是 NCHW，datatype 都是 uint8；crop、shift、affine 关闭；resize 使用 TensorFlow bilinear + half-pixel；pad 使用 constant 0。mean/std 不在 AI2D 中重复处理，模型/编译链负责相应预处理。

调用 `build_schedule()` 预编译 schedule，之后每帧只调用 `invoke()`。

## 4. 一帧推理流程

`process()` 的步骤如下：

1. 清空输出 `faces` 和 timing。
2. 对 VICAP CHN1 调用 `kd_mpi_vicap_dump_frame(..., VICAP_DUMP_YUV, ..., 200)`。
3. 验证 frame 是 1280×720、RGB888 planar、stride 等于 1280，三个 plane 物理地址连续。
4. 用 `kd_mpi_sys_mmap()` 映射三平面 RGB buffer。
5. 用 `hrt::create()` 将物理帧包装成 nncase runtime tensor，并执行 `hrt::sync(...sync_write_back...)`。
6. 记录 AI2D 起止时间并调用 `builder->invoke()`。
7. 记录 KPU 起止时间并调用 `model.run()`。
8. 读取 9 个 output tensor，复制到主机侧 `std::vector<float>`，解除 tensor map。
9. 将 9 个 head 交给 `decode_faces()`，完成 anchor 解码、阈值过滤、NMS 和坐标输出。
10. 解除映射并调用 `kd_mpi_vicap_dump_release()`。
11. 记录 total 时间并返回结果。

无论推理中途失败，函数都会尽量执行 `munmap` 和 `dump_release`，避免把 RGB buffer 永久占住。

## 5. 主循环和 AI 开关

`big/main.cpp` 中 `ai_enabled` 是主循环状态：

- 收到 `SetAi(1)` 时只打开处理状态，已有模型不重新 load。
- AI 打开后连续调用 `Detector::process()`。
- 成功时将检测结果交给 OSD，并更新 face count 和四项 timing。
- 每秒用已完成帧数/经过时间计算 `ai_fps_milli`。
- 推理失败时记录错误、关闭 AI、清除 OSD，摄像头和编码通道保持运行。
- AI 关闭时不 dump RGB 帧，并清除 OSD。

这使 AI 故障不会直接关闭 Camera 或 VENC，但也不会自动重试模型或吞掉错误。

## 6. 检测结果

`common/faces.cpp` 的 `decode_faces()` 使用模型输出 head 和预先生成的 4200 个 anchors，输出 `Face`：人脸框、5 个关键点和置信度。解码后再做阈值过滤和 NMS，坐标输出对应 1280×720 输入，后续由 OSD 做竖屏映射。

## 7. 当前边界

- 只加载 `retinaface.kmodel`，不使用 `mbface.kmodel`、特征库和身份识别。
- 统计的 KPU 时间是 `model.run()` 到输出 tensor 可读前的 runtime 区间，total 还包含 dump、映射和输出拷贝。
- 实际 KPU 速度、传感器输出和并行 AI FPS 必须上板测量；离线测试只能验证模型契约、解码和错误路径。
