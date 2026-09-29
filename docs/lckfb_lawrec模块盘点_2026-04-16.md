# lckfb lawrec 模块盘点

## 1. 目标

本文档用于落实收敛清单中的 `P0-3`：

- 明确当前 `lawrec` 代码里哪些模块是当前主线必须保留
- 哪些模块属于兼容保留
- 哪些模块是历史 `doorlock` 业务残留，后续应优先清理

本文档不讨论最终重构方案，只回答“当前为什么保留这些文件”。

## 2. 当前主线定义

当前 `lawrec` 的主线目标已经收敛为：

- 小核最终负责单进程主应用、UI、输入、页面切换、状态展示、RTSP 与控制状态机
- 大核负责显示底座、摄像头采集、预览控制、编码、IPC 被控执行
- 后续准备接入 RTSP、录像、取证等新业务

因此判断一个模块是否应保留，优先看它是否服务于：

- 大小核启动链
- 显示链
- IPC 链
- 预览链
- 后续 RTSP 扩展

## 3. 大核模块盘点

### 3.1 当前核心保留模块

这些文件属于当前主线必须保留项。

#### [big/main.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/main.cc)

保留原因：

- 当前大核主入口
- 承担显示初始化、摄像头初始化、预览控制、IPC 服务、运行循环
- 虽然职责过重，但目前仍是系统主链核心

后续动作：

- 不删除
- 必须逐步拆分职责

#### [big/vi_vo.h](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/vi_vo.h)

保留原因：

- 包含大核显示尺寸、OSD 参数、VO/OSD 帮助函数
- 直接参与当前显示链和预览链

后续动作：

- 保留
- 后续可并入 `display/preview` 模块

#### [big/CMakeLists.txt](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/CMakeLists.txt)

保留原因：

- 当前大核工程构建入口
- 决定可执行文件 `lawrec.elf`

后续动作：

- 保留
- 后续随着模块拆分增补源文件列表

### 3.2 当前条件保留模块

这些文件当前仍在编译链中，但不应继续扩大依赖范围。

#### [big/mobile_retinaface.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/mobile_retinaface.cc)

#### [big/mobile_retinaface.h](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/mobile_retinaface.h)

保留原因：

- 当前大核 AI 检测能力仍可作为后续能力保留
- 虽然现在主线不是继续调人脸识别，但该能力已经跑通过，不应贸然删除

后续动作：

- 保留
- 从“主循环强绑定”改为“按状态启用”

#### [big/mobile_face.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/mobile_face.cc)

#### [big/mobile_face.h](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/mobile_face.h)

保留原因：

- 历史上用于识别/特征比对
- 当前代码中仍有编译和条件调用路径

后续动作：

- 先保留
- 但默认不应再作为主线依赖

#### [big/model.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/model.cc)

#### [big/model.h](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/model.h)

#### [big/anchors_320.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/anchors_320.cc)

#### [big/util.cc](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/util.cc)

#### [big/util.h](/home/atlantis/k230_sdk/src/reference/business_poc/lawrec/big/util.h)

保留原因：

- 这些是当前 AI 模块的依赖文件

后续动作：

- 暂时保留
- 等 AI 主线与预览/RTSP 解耦后再评估是否需要继续收缩

### 3.3 当前历史业务残留热点

这些逻辑不是现在马上删除，而是明确标记为后续优先清理对象。

#### `main.cc` 中的人脸库共享内存流程

特征：

- `read_mem_feature()`
- `clear_feature()`
- `MSG_CMD_SIGNUP / IMPORT / DELETE / FEATURE_SAVE`
- `mem_feature_data`

判断：

- 这是典型 `doorlock` 业务残留
- 与当前“拍摄 / 预览 / RTSP”主线不一致

处理建议：

- 先保留兼容
- 但不要继续新增对这条链的依赖

#### `main.cc` 中的 `/sharefs/pic/` 导入流程

特征：

- `getFileNames()`
- 离线导图识别/入库逻辑

判断：

- 属于历史导入/人脸库业务

处理建议：

- 标记为后续待裁剪

#### `main.cc` 中大段 AI + 注册识别主循环

判断：

- 当前对 `lawrec` 来说不是最优先业务
- 现阶段应从主入口中逐步退居为可选模块

处理建议：

- 先保持可编译
- 后续改成显式开关

## 4. 小核模块盘点

### 4.1 当前核心保留模块

#### [src/ui/src/msg_proc.cpp](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/src/ui/src/msg_proc.cpp)

保留原因：

- 小核与大核 IPC 主入口
- 直接参与页面控制与状态同步

后续动作：

- 保留
- 继续收敛大小核 IPC 命令表
- 后续从“小核本地 socket 转发”演进为“单进程内直接调用控制模块”

#### [src/service/src/main.cpp](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/src/service/src/main.cpp)

保留原因：

- 当前过渡态里承担 RTSP 控制入口
- 已验证 RTSP 可以在该进程内直接启动，不再依赖外部 `lawrec_rtsp` 二进制

后续动作：

- 不再继续把它当成最终独立进程保留
- 后续应把其中控制逻辑收敛为单进程主应用内部模块

#### [src/rtsp/main.cpp](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/src/rtsp/main.cpp)

保留原因：

- 当前已承载可工作的 RTSP 入口
- 已补齐异步启停接口，可供上层模块内嵌调用

后续动作：

- 保留为 RTSP 模块
- 不再以独立可执行文件方向继续演化

#### [S99lawrec](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/S99lawrec)

保留原因：

- 小核 UI 启动脚本

后续动作：

- 保留
- 后续统一梳理启动链
- 最终应只启动一个小核主应用，而不是分别启动 UI 与 service

### 4.2 当前条件保留模块

#### [src/ui/src/scr_ota.c](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/src/ui/src/scr_ota.c)

保留原因：

- 当前仍在 UI 工程中存在
- 但不是当前主线重点

处理建议：

- 保留编译
- 但不作为当前业务重点

#### [src/xiaodemo/src/main.c](/home/atlantis/k230_sdk/src/little/buildroot-ext/package/lawrec/src/xiaodemo/src/main.c)

保留原因：

- 作为触控单独验证工具仍有参考价值

处理建议：

- 保留
- 不纳入正式主线能力

### 4.3 当前兼容保留项

#### IPC 服务名 `"door_lock"`

判断：

- 当前属于运行时兼容保留项，不是语义正确项

处理建议：

- 当前阶段不切
- 等大小核协议整理后统一切换

## 5. 后续删除/收缩优先级建议

建议的后续收缩顺序如下：

1. 先把大核 `main.cc` 中的人脸库管理逻辑从主循环中边缘化
2. 再把 `/sharefs/pic/` 导入链从主流程中独立出去
3. 再把 AI 检测/识别整理成可选模块
4. 最后再决定是否完全移除旧 `doorlock` 业务接口

## 6. 当前结论

当前可以明确得出三个结论：

1. `lawrec` 现在的真正底座是“显示链 + 预览链 + IPC 链”，不是 `doorlock` 的识别业务。
2. AI 和人脸库逻辑目前属于“条件保留能力”，不能再主导工程结构。
3. 后续所有新功能，尤其是 RTSP，必须建立在新的模块边界上，而不是继续叠加到旧业务主循环上。
