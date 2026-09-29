# lckfb lawrec 启动链文档

## 1. 目标

本文档落实 `P1-6` 和 `P2-4` 中的启动链文档要求。

## 2. 小核启动链

- little Linux 启动后进入 init
- 当前镜像中的 `/etc/init.d/S99lawrec` 仍会拉起小核 UI 与后台进程
- 当前脚本属于过渡态实现
- 当前脚本会：
  - 等待 `/dev/dri/card0`
  - 等待 `/dev/ipcm_user`
  - 停掉 legacy `touch_emu`
  - 启动 `/app/lawrec/ui/ui`
  - 启动 `/app/lawrec/service/lawrec_service`

## 2.1 小核正式目标启动链

- 自 2026-04-16 之后确认的正式方向，不再维持双进程 UI/service
- 最终应收敛为：
  - `/etc/init.d/S99lawrec` 只拉起一个小核主应用
  - 主应用内部完成 UI、控制、RTSP、大小核 IPC 初始化
- 正式目标启动链：
  - little Linux 启动后进入 init
  - `/etc/init.d/S99lawrec` 等待显示与 IPC 设备节点
  - `/etc/init.d/S99lawrec` 启动 `/app/lawrec/lawrec_app`
  - `lawrec_app` 内部初始化 UI 模块、control 模块、rtsp 模块

## 3. 大核启动链

- RT-Smart 启动后执行 `/bin/init.sh`
- 当前策略为：
  - 不自动启动 `fastboot_app.elf`
  - 仅打印手动调试提示

## 4. 当前手动调试入口

当前大核正确手动启动命令是：

```sh
/bin/fastboot_app.elf /bin/retinaface.kmodel /bin/mbface.kmodel
```

不是 `/sharefs/lawrec.elf`。

## 5. 当前结论

- 当前镜像中小核 UI 与 service 默认自启，属于过渡态
- 大核业务程序默认不自启
- 当前大核调试模式是“底层起来，但业务手动启动”
- 正式收敛方向是“小核单进程主应用 + 大核手动或后续按策略启动”
