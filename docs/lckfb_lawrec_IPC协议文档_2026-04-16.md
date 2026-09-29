# lckfb lawrec IPC 协议文档

## 1. 目标

本文档落实 `P1-3` 和 `P2-4` 中的 IPC 协议文档要求。

本文档只讨论大小核 IPC。

- 小核 UI 与后台之间的本地 socket 不属于后续正式 IPC 方向。
- 自 2026-04-16 之后确认的小核正式方向是单进程主应用，因此小核内部 UI/control/RTSP 之间不再定义独立 IPC 协议。

## 2. 当前运行时兼容约束

- 当前 IPC 服务名仍为 `door_lock`
- 这是历史兼容值，不代表最终项目命名

## 3. 当前已在用命令

- `MSG_CMD_PING`
- `MSG_CMD_PING_RESULT`
- `MSG_CMD_PREVIEW_ENTER`
- `MSG_CMD_PREVIEW_ENTER_RESULT`
- `MSG_CMD_PREVIEW_EXIT`
- `MSG_CMD_PREVIEW_EXIT_RESULT`

## 4. 当前兼容保留命令

- `MSG_CMD_SIGNUP`
- `MSG_CMD_SIGNUP_RESULT`
- `MSG_CMD_IMPORT`
- `MSG_CMD_IMPORT_RESULT`
- `MSG_CMD_DELETE`
- `MSG_CMD_DELETE_RESULT`
- `MSG_CMD_FEATURE_SAVE`
- `MSG_CMD_ERROR`

这些命令来自历史 `doorlock` 业务链，目前只作为兼容保留。

## 5. 后续建议新增命令

- `MSG_CMD_RTSP_START`
- `MSG_CMD_RTSP_START_RESULT`
- `MSG_CMD_RTSP_STOP`
- `MSG_CMD_RTSP_STOP_RESULT`
- `MSG_CMD_RTSP_QUERY`
- `MSG_CMD_RTSP_STATUS`
- `MSG_CMD_RECORD_START`
- `MSG_CMD_RECORD_STOP`

## 6. 协议演进原则

- 先保持服务名兼容
- 先扩命令，不先改底层通道名
- 等大小核都切到 `lawrec` 语义后，再统一切换服务名
- 不再把“小核 UI 与小核后台”之间的本地 socket 协议继续做大做重
