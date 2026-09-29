# lckfb lawrec 配置管理方案

## 1. 目标

本文档落实 `P2-2` 的配置管理要求。

## 2. 当前已收敛配置

当前已新增集中配置头：

- `big/include/lawrec_runtime_config.h`

当前先收敛了：

- `LAWREC_IPC_SERVICE_NAME`
- `LAWREC_BUILD_TAG`
- `LAWREC_STAGE0_PREVIEW_ONLY`
- `LAWREC_ENABLE_AI_PIPELINE`

## 3. 后续建议继续集中化的配置

- RTSP 默认 stream 名
- RTSP 默认端口
- 录像默认开关
- AI 默认开关
- 预览默认策略
- 页面进入拍摄后的默认动作

## 4. 原则

- 运行策略优先从“散落宏”收敛到集中头文件
- 板级差异配置和业务策略配置分开
- 不再让 `main.cc` 自己扩散新的全局宏
