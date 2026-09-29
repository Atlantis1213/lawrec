# lckfb lawrec 目录规范

## 1. 目标

本文档落实 `P2-3` 的目录规范要求。

## 2. 当前已建立的大核模块目录

- `big/include`
- `big/src/display`
- `big/src/ipc`
- `big/src/ai`
- `big/src/preview`
- `big/src/rtsp`
- `big/src/common`

## 3. 当前约束

- 新增大核功能优先落到对应模块目录
- 不再默认直接塞回 `big/main.cc`
- 文档统一保留在 `src/reference/business_poc/lawrec`

## 4. 后续目标结构

- `big/include`
- `big/src/*`
- `little/src/app`
- `little/src/ui`
- `little/src/control`
- `little/src/rtsp`
- `little/src/common`
- `docs`
- `scripts`

## 5. 当前结论

目录规范已开始落地，但代码仍处于过渡期。后续每次新增功能，都应优先向“单进程小核主应用 + 模块分层”这套结构收敛。
