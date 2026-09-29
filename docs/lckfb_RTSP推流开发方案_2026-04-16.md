# lckfb RTSP推流开发方案

## 工程原则

本项目后续 RTSP 功能必须遵守以下原则：

- 不允许以 shell 脚本作为 RTSP 主控制路径。
- 不允许以“UI 进程 + service 进程 + rtsp 子进程”作为最终正式架构。
- 不允许 UI 直接 `system()`、直接拉起或停止业务进程。
- RTSP 必须以内嵌模块方式接入业务主程序，不再保留独立 `lawrec_rtsp` 可执行文件作为正式形态。
- 大核不承载 RTSP server，不监听 RTSP socket，只继续负责摄像头、预览、编码侧能力。
- 小核最终应收敛为单应用主进程，UI、控制、RTSP 为同进程内部模块。
- 后续新增录像、网络状态、推流参数配置等能力时，统一接入主应用控制模块，不允许继续扩散本地 socket 控制链。

## 当前状态与目标状态

- 当前已验证过的过渡态：
  - 小核 `lawrec_service` 已经可以进程内启动 RTSP 模块。
  - UI 进入预览后可触发 RTSP 启动。
  - 板端 `8554` 端口已监听，客户端已能建立 RTSP 连接。
- 当前仍属于过渡态的部分：
  - 小核仍保留 `ui` 与 `lawrec_service` 双进程结构。
  - UI 与 service 之间仍走本地 socket。
- 自 2026-04-16 之后确认的正式收敛方向：
  - 小核最终只保留一个主应用进程。
  - RTSP 保留为主应用内部模块，不再保留独立 `lawrec_rtsp` 进程。
  - 本地 UI↔service IPC 不是最终架构，后续应删除。

## 1. 背景与目标

当前 `lawrec` 工程已经完成了以下基础能力：

- 小核 Linux 侧负责 UI、输入事件、页面切换、IPC 控制。
- 大核 RT-Smart 侧负责摄像头采集、显示链路、AI 任务和业务主循环。
- 当前“进入拍摄”页面已经能够通过 IPC 控制大核打开预览图层。

下一阶段目标是为项目增加 RTSP 推流能力，用于：

- 在 PC 或手机端实时查看设备画面。
- 为后续录像、远程取流、联调 AI 结果提供统一视频出口。
- 为将来的业务联动预留“本地预览 + 网络推流”双输出能力。

本方案只讨论 RTSP 推流链路，不扩展 UI 美化、触控修复等无关内容。

本轮建议目标分为三步：

1. 先验证小核 `mapi + live555` RTSP 能力能独立出流。
2. 再把 RTSP 收敛进 `lawrec` 小核业务主程序内部。
3. 最后删除小核 UI 与后台之间的本地进程边界，统一为单进程应用。

不建议一开始就做“向第三方平台主动推流”的 RTSP Pusher，因为那会把网络、鉴权、远端服务端兼容性一起引入，调试复杂度明显上升。

## 2. 总体架构建议

### 2.1 推荐架构

推荐采用如下分层：

- 小核 Linux `lawrec_app`
  - 单进程主应用。
  - 内部包含 UI 模块、控制模块、RTSP 模块。
  - 对外只保留一个进程入口。
- 小核内部 `ui module`
  - 负责页面与用户操作。
  - 只通过进程内接口调用控制模块。
  - 负责显示 RTSP 地址、运行状态、错误提示。
- 小核内部 `control module`
  - 负责预览、RTSP、录像等业务状态机。
  - 负责协调 UI 与大小核 IPC。
  - 负责统一管理 RTSP 生命周期。
- 小核内部 `rtsp module`
  - 负责 `mapi + live555`。
  - 负责 RTSP server 会话与推流循环。
  - 不承担页面、按键、UI 状态机。
- 大核 RT-Smart
  - 负责摄像头采集。
  - 负责预览显示链。
  - 负责编码所需的媒体侧能力。
  - 不负责 RTSP server socket。

### 2.2 主推荐方案

主推荐方案为：

`大核摄像头/编码能力 -> 小核 mapi client -> 小核 lawrec_app 内部 rtsp module(live555)`

也就是：

- 摄像头原始图像与预览仍由大核侧链路提供。
- 小核通过官方 `mapi` client API 接入编码输出。
- 小核主应用内部 RTSP 模块持有 RTSP server。
- 小核主应用控制模块负责正式调度，不使用脚本作为主入口。

### 2.3 明确禁止方案

禁止作为正式工程形态的方案：

- 在大核 RT-Smart 侧直接承载 RTSP server。
- 在 UI 里直接 `system()` 拉起 RTSP 进程。
- 保留 `UI -> lawrec-service -> lawrec_rtsp` 作为最终正式形态。
- 通过 `/app/*.sh` 脚本作为 RTSP 主控制路径。
- 直接在小核 Linux 重新接管摄像头采图。
- 首版直接做 AI 叠框推流。
- 首版直接做 RTSP Pusher 推到上级 NVR/服务器。

原因：

- 大核环境已证明不适合承担 RTSP socket server。
- UI 直接管进程会导致控制链分散、状态不可追踪。
- shell 方案只适合临时联调，不适合正式工程。
- 当前图像 owner 在大核，小核不适合重新接管视频数据。
- AI 叠框会引入 OSD/内存同步问题，首版应先求稳定出流。
- Pusher 依赖远端服务端可用性，不利于本地闭环调试。

## 3. 现有可复用能力

当前 SDK 内已经有可复用的编码与 RTSP 参考实现，优先复用，不建议从零实现协议层。

### 3.1 VENC 相关参考

- [sample_venc.c](/home/atlantis/k230_sdk/src/big/mpp/userapps/sample/sample_venc/sample_venc.c)
- [sample_av.c](/home/atlantis/k230_sdk/src/big/mpp/userapps/sample/sample_av/sample_av.c)

这些 sample 可用于参考：

- VENC 通道创建方式
- 编码参数设置方式
- 码流获取方式
- VB 池和 buffer 申请方式

### 3.2 RTSP 相关参考

- [main.cpp](/home/atlantis/k230_sdk/src/big/mpp/middleware/sample/sample_rtspserver/main.cpp)
- [main.cpp](/home/atlantis/k230_sdk/src/big/mpp/middleware/sample/sample_rtsppusher/main.cpp)
- [main.cpp](/home/atlantis/k230_sdk/src/big/mpp/middleware/sample/sample_rtspclient/main.cpp)
- [rtsp_server](/home/atlantis/k230_sdk/src/big/mpp/middleware/src/rtsp_server)
- [rtsp_pusher](/home/atlantis/k230_sdk/src/big/mpp/middleware/src/rtsp_pusher)

这些参考说明：

- SDK 已经内置 RTSP server/pusher 能力。
- 协议层无需自写，重点是把 `lawrec` 的编码帧正确接进去。

### 3.3 其他业务工程可参考封装

- [media.cpp](/home/atlantis/k230_sdk/src/reference/business_poc/peephole/big/media/src/media.cpp)

这个工程可参考：

- 业务工程如何封装媒体初始化
- 如何把 sample 代码收敛成工程模块

### 3.4 SDK 文档参考

- [README.md](/home/atlantis/k230_sdk/src/common/cdk/user/README.md)
- [开发准则.md](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/开发准则.md)
- [lckfb_显示与推理链移植手册_2026-04-10.md](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/lckfb_%E6%98%BE%E7%A4%BA%E4%B8%8E%E6%8E%A8%E7%90%86%E9%93%BE%E7%A7%BB%E6%A4%8D%E6%89%8B%E5%86%8C_2026-04-10.md)

## 4. 开发目标拆解

建议把 RTSP 能力拆成四个阶段。

### 阶段 1：验证小核 RTSP 能力

目标：

- 不接小核 UI。
- 不接复杂状态机。
- 先验证 `mapi + live555` 组合可正常出流。
- PC 端可以直接 `ffplay rtsp://<board_ip>/lawrec` 拉流。

验收标准：

- 能看到实时画面。
- 客户端反复断开/重连后服务端仍稳定。
- 不影响现有大核预览基础链路。

### 阶段 2：收敛成 `lawrec` 自有模块

目标：

- 把 RTSP 能力从 sample 风格代码收敛成 `lawrec` 内部 RTSP 模块。
- 提供清晰的 `start/stop/query` 接口。

验收标准：

- 主循环不直接散落大量媒体细节代码。
- 推流状态有明确日志。
- 资源释放后可重复启动。

### 阶段 3：接入小核业务主程序

目标：

- 把 RTSP 模块接入小核主程序内部。
- UI 不再跨进程访问 RTSP。
- 控制模块统一管理 `RTSP_START/STOP/QUERY`。
- UI 展示 RTSP 地址和运行状态。

验收标准：

- 进入页面后可拉流。
- 返回页面后推流停止。
- UI 不直接执行 shell。
- 重复进入/退出不崩溃、不泄漏。
- 小核正式形态不再依赖本地 socket 控制 RTSP。

### 阶段 4：增强功能

可选扩展：

- RTSP Pusher
- 音频
- 码率动态配置
- OSD 叠字/叠框
- 鉴权
- 断网重连

## 5. 代码落点建议

### 5.1 小核工程建议新增模块

建议在 `lawrec` 小核工程中收敛如下模块：

- `src/app/`
- `src/ui/`
- `src/control/`
- `src/rtsp/`
- `src/common/`

职责建议如下：

- `app`
  - 单进程主入口
  - 负责初始化 UI、control、rtsp、big-core IPC
- `control`
  - 统一管理预览、RTSP、录像等业务状态
  - 对 UI 提供进程内接口
- `rtsp`
  - 承载 `mapi + live555`
  - 管理 stream session
  - 负责发送实际 RTP/RTSP 数据
- `common protocol`
  - 统一命令字、状态结构、版本号
  - 作为小核内部模块与大小核 IPC 的共享契约

### 5.2 主程序中的状态机

主程序建议只保留状态协调，不直接操作底层编码细节。

建议新增运行状态：

- `PREVIEW_DISABLED`
- `PREVIEW_ENABLED`
- `RTSP_DISABLED`
- `RTSP_ENABLED`

如果后面需要更清晰，也可以合并成组合状态：

- `IDLE`
- `PREVIEW_ONLY`
- `RTSP_ONLY`
- `PREVIEW_AND_RTSP`

### 5.3 控制接口建议

接口分两层：

- 大小核 IPC
  - 仍保留预览相关命令
  - RTSP 不再经由大核承载 server
- 小核进程内控制接口
  - `RTSP_START`
  - `RTSP_STOP`
  - `RTSP_QUERY`
  - `RTSP_STATUS`

其中：

- `START/STOP` 用于控制主应用内部 RTSP 模块。
- `QUERY/STATUS` 用于 UI 页面进入后同步当前状态。
- 不再把 Unix domain socket 作为最终正式控制方式。

## 6. 视频链路设计建议

### 6.1 编码源选择

推荐优先使用与当前预览同源的摄像头采集链，但编码和显示尽量分离为两个输出方向。

建议思路：

- 一个 sensor 输入
- 保持现有 VO 预览输出
- 额外开一路 VENC 编码输出

如果当前采集链支持多通道输出，优先使用独立通道给 VENC，避免直接复用 VO 输出结果。

### 6.2 首版分辨率建议

建议首版优先：

- `1280x720 @ 15fps`

原因：

- 带宽、码率、编码负载更稳妥
- 对 Wi-Fi 调试更友好
- 足够验证链路

后续再评估：

- `1920x1080 @ 15fps`

### 6.3 编码参数建议

首版建议：

- 编码格式：H.264
- Profile：Main 或 Baseline
- 帧率：15fps
- GOP：30
- 码率控制：CBR
- 码率：
  - 720p：2Mbps 起步
  - 1080p：4Mbps 起步

不建议首版就使用：

- H.265
- B 帧
- 超高码率
- 动态码率自适应

### 6.4 关键帧策略

建议保证：

- RTSP 客户端新连接时能尽快拿到 IDR
- 支持周期性关键帧

否则会出现：

- VLC/ffplay 连接后长时间黑屏
- 断线重连后迟迟不出图

## 7. RTSP 组件接入方式

### 7.1 方案 A：先直接参考 `sample_rtspserver`

优点：

- 上手最快
- 最适合阶段 1 打样

缺点：

- sample 风格代码通常较散，不适合直接长期保留在业务主线

适用阶段：

- 阶段 1

### 7.2 方案 B：复用 middleware 封装，落到 `lawrec`

优点：

- 架构更清晰
- 更利于后续加状态机、IPC、错误恢复

缺点：

- 需要先理解 middleware 的接口边界

适用阶段：

- 阶段 2 及以后

### 7.3 最终建议

建议采用“两步走”：

1. 先复用 sample 思路跑通编码帧送 RTSP。
2. 再把逻辑整理进 `lawrec` 自己的小核 `rtsp` 模块，并由单进程主应用统一调度。

## 8. 与当前 lawrec 架构的集成方式

### 8.1 小核职责

小核正式职责：

- 页面入口
- 显示推流状态
- RTSP server
- 小核控制状态机
- 与大核的 IPC 控制

不要让小核参与：

- 摄像头主数据面 owner
- 编码 owner
- 与大核重复建链的采图流程

### 8.2 大核职责

大核负责：

- 摄像头采集
- 编码
- 向小核 RTSP 提供可消费编码能力
- 资源释放
- 错误日志

### 8.3 与“进入拍摄”页面的关系

建议后续和“进入拍摄”页面这样联动：

- 进入拍摄页面：
  - 打开预览
  - 根据页面开关决定是否启动 RTSP
- 返回主页面：
  - 关闭 RTSP
  - 关闭预览或恢复空白层

首版为了降低复杂度，也可以先不和页面强绑定，而是先增加一个“开始推流/停止推流”单独入口。

## 9. 资源与冲突评估

### 9.1 与预览的关系

当前大核显示链不能关闭，因为它本身就是屏幕驱动链的一部分。

因此 RTSP 设计时应遵循：

- 不关闭现有大核显示底层链
- 只控制摄像头预览图层和编码链

### 9.2 与 AI 推理的关系

当前大核还存在推理链历史包袱。

建议 RTSP 首版明确策略：

- 首版默认关闭 AI 推理输出
- 不叠加框、不做人脸识别结果叠图
- 先确保纯视频推流稳定

否则会引入：

- 多线程资源竞争
- 帧率下降
- 内存池不足
- VO/VENC/AI 三方同步问题

### 9.3 VB 池和 buffer 风险

首版需要重点评估：

- VENC 额外 buffer 数量
- RTSP 环形缓存深度
- 预览与编码共存时总内存占用

建议在阶段 1 就记录：

- 编码初始化成功率
- 客户端连接后帧率
- 断开重连后的内存是否回收

### 9.4 网络风险

RTSP 验证阶段建议优先使用：

- 有线网络
- 稳定热点
- 局域网内 PC

不要一开始就拿弱 Wi-Fi 环境做稳定性结论。

## 10. 推荐实施顺序

### 10.1 M1：验证小核 RTSP 模块能力

工作内容：

- 在小核 RTSP 模块里完成最小 RTSP 初始化逻辑。
- 固定启动一条 `lawrec` stream。
- PC 侧用 `ffplay`/VLC 拉流验证。

建议输出：

- 串口日志打印：
  - RTSP server init ok
  - stream path
  - mapi/venc init ok
  - client connected/disconnected

### 10.2 M2：整理为独立模块

工作内容：

- 把 VENC 和 RTSP 逻辑拆到独立 `.h/.cpp`
- 增加 `start()/stop()/is_running()` 接口

建议输出：

- 可以重复 start/stop
- 出错时有明确返回码

### 10.3 M3：接入控制模块

工作内容：

- UI 通过进程内接口触发 `RTSP_START/STOP`
- 小核控制模块根据状态切换 RTSP
- 仅在需要联动预览/编码时再通过大小核 IPC 通知大核

建议输出：

- 小核控制模块打印明确日志
- 小核页面显示当前 RTSP URL

### 10.4 M4：接入正式页面

工作内容：

- RTSP 控制与“进入拍摄”联动
- 返回时释放资源

建议输出：

- 从主界面进入拍摄后可拉流
- 返回后出流停止

## 11. 建议的验证步骤

### 11.1 阶段 1 验证

板端启动大核程序后，在 PC 端执行：

```bash
ffplay rtsp://<board_ip>/lawrec
```

或：

```bash
ffprobe rtsp://<board_ip>/lawrec
```

也可以用 VLC / PotPlayer。

验证项：

- 是否能成功连接
- 是否能持续出画
- 是否存在明显花屏
- 首帧时间是否可接受
- 客户端断开重连是否正常

### 11.2 阶段 2 验证

重复执行：

1. 启动推流
2. 停止推流
3. 再次启动

验证项：

- 资源是否释放干净
- 第二次启动是否成功
- 是否有 VENC/RTSP 重复初始化报错

### 11.3 阶段 3 验证

在 UI 页面反复操作：

1. 进入页面
2. 开始推流
3. 返回页面
4. 再次进入

验证项：

- 小核状态显示是否同步
- 大核是否正确收到 IPC
- 页面返回后是否停止推流

## 12. 推荐的日志与调试点

建议新增以下日志，不要过多刷屏，但要足够定位问题。

### 大核日志建议

- 预览进入/退出
- 编码初始化成功
- 编码帧首帧到达
- stop 时资源释放完成

### 小核日志建议

- RTSP server 创建成功
- stream 名称和 URL
- 发送 RTSP_START/STOP 控制动作
- 页面显示的 URL
- RTSP 客户端连接/断开

## 13. 不建议现在就做的事情

以下内容不建议和首版 RTSP 同时推进：

- 触控问题继续深入排查
- UI 重布局
- 视频录像功能
- AI 检测框叠加到推流
- 音频同步
- 云端平台对接

原因是这些事项会显著放大变量，不利于把 RTSP 主链路先闭环。

## 14. 最终推荐方案

最终推荐顺序如下：

1. 小核先基于现有 `mapi + live555` 能力验证一条 `RTSP Server` 最小闭环。
2. 先固定 `rtsp://<board_ip>/lawrec`，不做复杂配置。
3. 首版只做 H.264 720p 15fps。
4. 首版不叠加 AI，不做录像，不做 pusher。
5. 稳定后再把 UI、control、RTSP 收敛成单进程主应用。

这样做的好处是：

- 风险最低
- 最接近现有架构
- 最容易快速验证
- 后续可平滑扩展到“进入拍摄页面自动开启推流”

## 15. 后续实施建议

建议下一步直接按下面顺序开始开发：

1. 先阅读并比对以下参考实现：
   - [sample_venc.c](/home/atlantis/k230_sdk/src/big/mpp/userapps/sample/sample_venc/sample_venc.c)
   - [main.cpp](/home/atlantis/k230_sdk/src/big/mpp/middleware/sample/sample_rtspserver/main.cpp)
   - [media.cpp](/home/atlantis/k230_sdk/src/reference/business_poc/peephole/big/media/src/media.cpp)
2. 在小核 `lawrec` 工程里收敛 `app/control/rtsp` 三个模块。
3. 先实现小核本地固定出流。
4. 验证通过后，再把 UI 改为进程内控制接口。

---

文档结论：

- 本项目 RTSP server 不应再以大核为正式落点。
- 首版主线应为“板端 RTSP Server”，不是 RTSP Pusher。
- 开发策略应遵循“先小核 RTSP 闭环，再收敛单进程主应用，再做业务联动”。
