# RT-Smart IPC 控制

## 1. 两段 IPC

控制链路为：

```text
demo_ui --Unix SOCK_SEQPACKET--> media_service --kd_ipcmsg--> vision.elf
```

UI 不直接连接 RT-Smart。媒体后台把预览/AI 命令转成 SDK IPC，并把 VENC、RTSP、录像状态汇总后回复 UI。

## 2. 线协议

定义在 `common/protocol.h`：

```cpp
Request = magic, version, bytes, id, command, value; // 24 bytes
Status  = magic, version, bytes, id, result, flags, busy,
          detections, AI timings, media rates, queues, RSS, VB, errors; // 84 bytes
```

字段全部是 32 位整数，没有指针和 `std::string`。编译期 `static_assert` 固定结构体大小。命令为 `GetStatus=0`、`SetPreview=1`、`SetAi=2`、`SetRtsp=3`、`SetRecord=4`；flags 表示已启用功能，busy 表示正在处理的命令。

`decode_request()` 校验 magic、version、bytes、command 范围和值只能为 0/1；`valid_status()` 校验响应 id、长度和 flags。

## 3. RT-Smart 端 `VisionControl`

`VisionControl::start()` 注册 `lawrec_demo` service，使用 SDK IPC port 102 连接 Linux remote id。接收线程等待连接建立后调用 `kd_ipcmsg_run()`。

SDK callback 只做以下事情：

1. 忽略 response 消息。
2. 校验 body、module、command。
3. 查询命令立即返回当前 status。
4. 设置命令保存到 pending slot，保存 response，并设置对应 busy 位。

摄像头和 AI 的实际变化不在 callback 中执行。视觉主循环调用 `control.take()`，修改 Camera/AI 状态后调用 `control.finish()` 填入状态并发送 response。这样硬件变更全部在主循环串行执行；同一时间只允许一个 pending 设置命令，其他设置返回 `-EBUSY`。

## 4. Linux 端 `VisionClient`

`little/media/vision_client.cpp` 注册同名服务并连接 remote id 1。`exchange()` 创建 SDK message，发送同步请求并等待 700 ms，验证 response module、command、Status id、size 和 `s32RetVal` 后复制 status。连接失败、超时、response 格式错误都返回 errno。

## 5. Linux UI socket

`media_service` 创建 `/var/run/lawrec-demo.sock`：

- `SOCK_SEQPACKET` 保留 Request 的消息边界。
- `SOCK_NONBLOCK|SOCK_CLOEXEC` 避免永久阻塞。
- `.lock` 文件通过 `flock(LOCK_EX|LOCK_NB)` 限制单实例。
- 旧路径只有在确认为 socket 时才删除；普通文件和符号链接会拒绝启动。
- socket 权限设为 0600。

`common/socket.cpp` 对 connect、send、recv 使用一个统一 deadline；收到的 status 必须是 84 bytes 且 id 匹配。

## 6. 命令路由

媒体后台收到 UI 请求后：

- Preview/AI：调用 `VisionClient::exchange()`。
- RTSP：先查询 vision 状态，再调用 `RtspWorker::request()`。
- Record：先查询 vision 状态，再调用 `Recorder::request()`。
- RTSP/Record stop 不要求新的 vision ACK，避免停止时依赖已经关闭的 peer。

后台把 vision flags 与 RTSP/Recorder state 合并，生成统一 flags、busy、队列和错误字段返回 UI。

## 7. IPC transport workaround

`common/ipc_transport.cpp` 用 linker wrap 替换 SDK 的非阻塞 `IPCMSG_TransConnect`：打开 `/dev/ipcm_user`，通过 ioctl 初始化属性和 try-connect；SDK 返回 peer pending 时额外查询 driver 状态，只接受 CONNECTING/CONNECTED，再把 fd 写回 SDK attrs。`static_assert` 检查当前 SDK ABI 结构为 44 bytes。这是当前 SDK 的适配，不是通用重连框架。
