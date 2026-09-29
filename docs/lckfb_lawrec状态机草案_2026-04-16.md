# lckfb lawrec 状态机草案

## 1. 目标

本文档落实 `P1-2`，定义后续 `lawrec` 应统一使用的状态边界。

## 2. 基础布尔状态

- `preview_backend_ready`
- `preview_enabled`
- `ai_enabled`
- `rtsp_enabled`
- `record_enabled`

## 3. 推荐组合态

- `IDLE`
- `PREVIEW_ONLY`
- `PREVIEW_AND_AI`
- `PREVIEW_AND_RTSP`
- `PREVIEW_AND_RECORD`

## 4. 当前约束

- `preview_backend_ready = 0` 时，任何预览/RTSP/录像都不能进入运行态
- `preview_enabled = 0` 时，实时摄像头路径应快速空转
- `ai_enabled` 不再默认跟随 `preview_enabled`
- `rtsp_enabled` 不再默认跟随页面切换
- `record_enabled` 不再默认跟随进入拍摄

## 5. 当前落地情况

当前代码已先落地：

- 预览 backend ready
- 预览 enabled
- AI enable helper

后续 RTSP / 录像接入时，应直接扩展这套状态机，而不是新增散落布尔变量。
