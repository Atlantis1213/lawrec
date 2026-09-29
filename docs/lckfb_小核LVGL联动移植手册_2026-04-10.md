# lawrec 小核 LVGL 联动移植手册

## 1. 目标

在 `k230_canmv_lckfb_defconfig` 上接入小核 Linux 侧 LVGL UI，并与大核 `lawrec` 通过 IPC 联动。

本轮目标是先跑通以下链路：

1. 小核 rootfs 中包含 `lawrec` LVGL 应用
2. 小核启动后自动拉起 LVGL UI
3. 小核 UI 继续使用 `door_lock` IPC 服务名，与当前大核 `lawrec` 保持兼容
4. 不破坏原始 `door_lock` 包和其他板级配置

## 2. 当前结论

### 2.1 为什么之前小核 UI 没起来

不是 UI 源码本身有问题，而是 `lckfb` 实际走的是通用 buildroot 配置：

- 顶层 defconfig:
  - `configs/k230_canmv_lckfb_defconfig`
- 默认 board name:
  - `CONFIG_BOARD_NAME="k230_evb"`
- 于是 buildroot 默认会落到：
  - `src/little/buildroot-ext/configs/k230_evb_defconfig`

而 `k230_evb_defconfig` 里虽然有：

- `BR2_PACKAGE_LVGL=y`
- `BR2_PACKAGE_TOUCH_EMU=y`

但没有：

- `BR2_PACKAGE_DOOR_LOCK=y`
- `BR2_PACKAGE_LAWREC=y`

所以之前镜像里根本没有小核 UI 程序，自然也没有联动。

### 2.2 稳妥方案

不要直接污染：

- `package/door_lock`
- `configs/k230_evb_defconfig`

而是新建一套 `lawrec` 小核包和专用 buildroot defconfig，让 `lckfb` 显式切到它。

## 3. 本轮实际改动

### 3.1 新增小核包

从原包复制得到：

- 原包：
  - `src/little/buildroot-ext/package/door_lock`
- 新包：
  - `src/little/buildroot-ext/package/lawrec`

保留了原始 UI、触控、IPC 处理逻辑，先只做最小必要修改：

- Buildroot 包名改为 `LAWREC`
- CMake `project()` 改为 `lawrec`
- 安装前缀改为：
  - `/app/lawrec`

最终安装结果为：

- 可执行程序：
  - `/app/lawrec/ui/ui`
- 资源目录：
  - `/app/lawrec/ui/data`

### 3.2 IPC 兼容策略

当前大核 `lawrec` 还保持：

- IPC 服务名：`door_lock`

所以小核 `lawrec` 包此轮也继续保持原始 IPC 服务名：

- `kd_ipcmsg_connect(..., "door_lock", ...)`
- `kd_ipcmsg_add_service("door_lock", ...)`

这样能保证先联通，不必同时改大核和小核的服务名。

### 3.3 新增 buildroot 包入口

在：

- `src/little/buildroot-ext/Config.in`

增加：

- `source "$BR2_EXTERNAL_K230_PATH/package/lawrec/Config.in"`

### 3.4 新增 lckfb 专用 buildroot defconfig

新增：

- `src/little/buildroot-ext/configs/k230_evb_lawrec_defconfig`

它基于 doorlock 所需依赖收敛出本轮最关键的小核 UI 依赖：

- `BR2_PACKAGE_LAWREC=y`
- `BR2_PACKAGE_LVGL=y`
- `BR2_PACKAGE_LIBDISP=y`
- `BR2_PACKAGE_TOUCH_EMU=y`
- `BR2_PACKAGE_OPENSSH=y`

### 3.5 顶层 lckfb defconfig 显式切换 buildroot 配置

修改：

- `configs/k230_canmv_lckfb_defconfig`

新增：

- `CONFIG_BUILDROOT_DEFCONFIG="k230_evb_lawrec"`

这是本轮最关键的一步，否则 `lckfb` 还是会继续走默认的 `k230_evb_defconfig`。

### 3.6 新增小核自启动脚本

新增：

- `src/little/buildroot-ext/package/lawrec/S99lawrec`

通过 `src.mk` 中的 `POST_INSTALL_TARGET_HOOKS` 安装到：

- `/etc/init.d/S99lawrec`

行为：

1. 开机时尝试启动 `touch_emu`
2. 后台拉起 `/app/lawrec/ui/ui`
3. 记录 pid 到：
   - `/var/run/lawrec-ui.pid`
4. 日志输出到：
   - `/tmp/lawrec-ui.log`

## 4. 本轮构建中踩到的问题

### 4.1 新包名会触发重复下载

因为包名从 `door_lock` 变成 `lawrec`，buildroot 会把 `lvgl v8.3.1` 视为新下载项，路径从：

- `dl/door_lock/v8.3.1.tar.gz`

变成：

- `dl/lawrec/v8.3.1.tar.gz`

解决方式：

1. 直接复用已有缓存：
   - 把 `door_lock` 的 `v8.3.1.tar.gz` 复制到 `dl/lawrec/`
2. 新增：
   - `src/little/buildroot-ext/package/lawrec/lawrec.hash`

这样 buildroot 会直接校验并复用本地缓存，不再卡在重复下载。

### 4.2 `CONFIG_MEM_FACE_DATA_BASE` 在 lckfb 下未定义

`door_lock` 的小核代码 `db_proc.c` 会在生成人脸库升级头时用到：

- `CONFIG_MEM_FACE_DATA_BASE`

但 `lckfb` 当前这套配置不是 SPI NOR face-db 分区场景，`k_autoconf_comm.h` 里没有该宏，导致编译失败。

报错文件：

- `src/little/buildroot-ext/package/lawrec/src/ui/src/db_proc.c`

解决方式：

在 `db_proc.c` 里增加兼容 fallback：

- `CONFIG_MEM_FACE_DATA_BASE -> 0x3FB00000`
- `CONFIG_MEM_FACE_DATA_SIZE -> 0x00040000`

这与当前大核 `lawrec` 里的人脸库共享内存基址保持一致。

### 4.3 这不等于 face_db 持久化已经完全验证

虽然这样能通过编译并与当前大核地址策略对齐，但要注意：

1. 小核 `feature_db_save()` 里仍然存在写 MTD 分区逻辑
2. 如果当前介质不是原始 doorlock 的 flash 分区方案，注册/导入/删除的人脸库持久化链还需要单独验证
3. 本轮优先目标是：
   - 小核 LVGL 跑起来
   - 与大核 IPC 联动
   - UI 按键链路先通

后续如果要完整复刻 doorlock 的人脸库持久化，需要继续核对：

- `/proc/mtd` 是否存在 `face` 分区
- `build-image` 生成的镜像里是否为 face_db 预留了对应分区
- 大核/小核是否统一使用当前共享内存 + 存储回写策略

## 5. 已验证结果

已通过 Docker 在 SDK 根目录执行：

```bash
make CONF=k230_canmv_lckfb_defconfig buildroot
```

验证到以下结果：

1. buildroot 配置中已经启用：
   - `BR2_PACKAGE_LAWREC=y`
   - `BR2_PACKAGE_LVGL=y`
   - `BR2_PACKAGE_LIBDISP=y`
   - `BR2_PACKAGE_TOUCH_EMU=y`
2. target rootfs 中已经存在：
   - `/app/lawrec/ui/ui`
   - `/app/lawrec/ui/data/img/*.png`
3. target rootfs 中已经存在：
   - `/etc/init.d/S99lawrec`

说明“小核包接入 + buildroot 编译 + target 安装 + 自启动脚本落地”这条链已经打通。

## 6. 关键文件清单

### 顶层配置

- `configs/k230_canmv_lckfb_defconfig`

### 小核 buildroot 配置

- `src/little/buildroot-ext/Config.in`
- `src/little/buildroot-ext/configs/k230_evb_lawrec_defconfig`

### 小核 lawrec 包

- `src/little/buildroot-ext/package/lawrec/Config.in`
- `src/little/buildroot-ext/package/lawrec/src.mk`
- `src/little/buildroot-ext/package/lawrec/lawrec.hash`
- `src/little/buildroot-ext/package/lawrec/S99lawrec`
- `src/little/buildroot-ext/package/lawrec/src/CMakeLists.txt`
- `src/little/buildroot-ext/package/lawrec/src/ui/src/msg_proc.cpp`
- `src/little/buildroot-ext/package/lawrec/src/ui/src/db_proc.c`

## 7. 后续建议执行顺序

### 7.1 先出一版整镜像

建议在 Docker 中继续执行完整镜像生成，例如：

```bash
make CONF=k230_canmv_lckfb_defconfig rt-smart-apps big-core-opensbi build-image
```

如果本轮还改了大核 `lawrec`，则按当前项目的大核发布流程一并打整卡镜像。

### 7.2 板端优先验证这几项

1. 小核启动后是否自动出现 LVGL UI
2. 触控是否正常
3. 小核 UI 点击后，大核是否收到对应 `door_lock` IPC 命令

## 8. 2026-04-11 联动阶段新增结论

这一轮不再只是“把小核 UI 包塞进 rootfs”，而是继续把 `doorlock` 式的显示协作和 IPC 联动往前推进了一步。

### 8.1 必须先理解 doorlock 的显示归属

`lckfb` 上当前要复刻的不是“小核单独点屏”，而是 `doorlock` 那种同一套显示硬件上的分层协作：

- 大核负责：
  - `VO + connector + panel + DSI` 初始化
  - 预览层
  - AI OSD
- 小核负责：
  - 通过 `/dev/dri/card0` 往 DRM plane 提交 LVGL framebuffer
  - 把 UI 叠加到大核已经初始化好的显示底座上

也就是说：

- 小核不负责实际屏幕硬件驱动
- 如果大核没有先把显示底座初始化完成，小核即使提交了 DRM plane，也可能看不到 UI

这个认识非常关键，否则移植方向会完全跑偏。

### 8.2 小核 LVGL 端本轮新增改动

为了让 `lawrec` 更像当前 `lckfb` 场景下的产品验证版本，小核 UI 侧新增了几类改动：

1. UI 可见性增强
   - 主界面增加了半透明底部 panel
   - 增加标题：
     - `LAWREC LVGL OVERLAY`
   - 增加状态文本：
     - `IPC connecting...`
     - `IPC connected`
     - `IPC ready`
     - `IPC error`

2. IPC 心跳验证
   - 新增：
     - `MSG_CMD_PING`
     - `MSG_CMD_PING_RESULT`
   - 小核 IPC 连接成功后会主动发一次 `PING`
   - 收到 `PING_RESULT` 后把状态更新成 `IPC ready`

3. `lckfb` 显示尺寸适配
   - 不再沿用 `doorlock` 固定的 `1080 x 960` 半屏假设
   - `lckfb` 下改成：
     - `display_width = screen_width`
     - `display_height = screen_height`
   - 仍保留非 `lckfb` 的 `doorlock` 下半屏逻辑

4. `lckfb` 触控映射适配
   - 把原来硬编码的输入映射改成可配置模式：
     - `MAP_MODE_DOORLOCK_HALF`
     - `MAP_MODE_CLAMP_ONLY`
   - `lckfb` 下使用整屏 clamp，不再强制 half-screen y-offset

### 8.3 大核联动新增改动

大核 `lawrec` 侧为了配合小核联动，补了 IPC 兼容和验证能力：

- 保持 IPC 服务名仍为：
  - `door_lock`
- 修复了发送长度错误：
  - 原来错误地发送了 `sizeof(content)`
  - 现在改成真实 `content_size`
- 支持：
  - `MSG_CMD_PING`
  - `MSG_CMD_PING_RESULT`
- 加入了更直接的日志：
  - `[lawrec] ipc ping`
  - `[lawrec] ipc import path=...`
  - `[lawrec] ipc signup name=...`
  - `[lawrec] ipc delete`

### 8.4 本轮非常重要的构建坑

这次联动阶段发现了一个很容易误判的问题：

- `make CONF=k230_canmv_lckfb_defconfig buildroot` 虽然可能成功
- 但不代表 `lawrec` 小核包一定被完整重编

实际踩到的情况是：

1. 源码已经改了
2. `buildroot` 目标成功了
3. 但旧的 `lawrec` 可执行文件仍然可能留在 target 里
4. 直到执行包级强制重编，才真正暴露出新的编译错误

这次真实暴露出来的错误是：

- `scr_main.c` 使用了 `lv_font_montserrat_20`
- `scr_main.c` 使用了 `lv_font_montserrat_16`
- 但当前 `lv_conf.h` 里这两个字体默认是关闭的

也就是说，如果不做包级强制重编，会出现一种非常危险的假象：

- “buildroot 成功”
- 但“新 UI 改动根本没有真的进镜像”

因此在后续移植里，建议把下面这个动作作为固定步骤：

```bash
docker run --rm --user 1000:1000 \
  -v /home/atlantis/k230_sdk:/home/atlantis/k230_sdk \
  -v /home/atlantis/k230_sdk/toolchain:/opt/toolchain \
  -w /home/atlantis/k230_sdk \
  k230_docker:latest \
  bash -lc 'make -C output/k230_canmv_lckfb_defconfig/little/buildroot-ext lawrec-rebuild'
```

注意：

- 不要在宿主机直接跑 `lawrec-rebuild`
- 宿主机直接跑时可能会使用错误的本机 CMake 路径，导致：
  - `cmake_check_build_system` 失败

必须放在 Docker 里执行，才能和整套 SDK 交叉编译环境保持一致。

### 8.5 本轮最终验证到的事实

#### 8.5.1 小核 UI 新文案已经进了可执行文件

通过对未 strip 的构建产物执行 `strings`，已经能直接看到：

- `LAWREC LVGL OVERLAY`
- `IPC connected`
- `IPC ready`
- `IPC error`
- `IPC connect fail`

这说明：

- 不是只有源码改了
- 新的 overlay/status 逻辑已经真正进入小核 `ui` 可执行文件

#### 8.5.2 大核 ELF 已包含联动日志

通过对大核 ELF 执行 `strings`，已经确认包含：

- `[lawrec] ipc ping`
- `door_lock`
- `[lawrec] detect boxes=%zu`

这说明：

- 大核联动分支也已进入当前产物

#### 8.5.3 整镜像已经重新生成

在 Docker 中执行：

```bash
make CONF=k230_canmv_lckfb_defconfig rt-smart-apps big-core-opensbi build-image
```

已经成功生成新的整卡镜像，关键产物包括：

- `output/k230_canmv_lckfb_defconfig/images/sysimage-sdcard.img`
- `output/k230_canmv_lckfb_defconfig/images/little-core/linux_system.bin`
- `output/k230_canmv_lckfb_defconfig/images/big-core/rtt_system.bin`

本轮一个可直接使用的镜像时间戳参考为：

- `sysimage-sdcard.img`
  - `2026-04-11 01:39:27 +0800`

### 8.6 下一次在新环境里建议的验证顺序

如果换到一个全新的环境，要快速判断“小核 LVGL 叠加 + 大小核 IPC 联动”是否真的通了，建议按下面顺序检查：

1. 先确认镜像里确实包含小核 UI
   - 检查 rootfs:
     - `/app/lawrec/ui/ui`
     - `/etc/init.d/S99lawrec`

2. 再确认小核 UI 产物里真的带了新字符串
   - 对未 strip 的 build 产物执行 `strings`
   - 搜索：
     - `LAWREC LVGL OVERLAY`
     - `IPC ready`

3. 再确认大核 ELF 已带联动逻辑
   - 搜索：
     - `[lawrec] ipc ping`
     - `door_lock`

4. 上板后先看最基础现象
   - 大核预览正常
   - 小核 LVGL panel 能叠加出来
   - 不要求先点按钮，只看开机后状态是否能从：
     - `IPC connecting...`
     - 变成 `IPC connected`
     - 最终到 `IPC ready`

5. 最后再测交互
   - 点击 UI 按钮
   - 看大核串口是否出现：
     - import
     - signup
     - delete
     - ping

如果第 4 步就失败，不要先怀疑业务逻辑，应优先回到以下几个方向排查：

- 大核显示底座是否真的先初始化完成
- 小核 `lawrec` 是否真的被重编并进了镜像
- 小核是否真的通过 `/dev/dri/card0` 在正确 plane 上提交了 framebuffer
- IPC 服务名是否仍然一致为 `door_lock`
4. 大核回消息后，小核 UI 是否有结果反馈

### 7.3 如果 UI 起不来，优先看下面几个点

1. `rootfs` 中是否真的有：
   - `/app/lawrec/ui/ui`
2. `init.d` 中是否真的有：
   - `/etc/init.d/S99lawrec`
3. 查看日志：
   - `/tmp/lawrec-ui.log`
   - `/tmp/touch_emu.log`
4. 手工启动：

```bash
/etc/init.d/S99lawrec restart
```

5. 如果仍无显示，继续核对：
   - `/dev/dri/card0`
   - `/dev/input/event0`
   - `touch_emu` 是否正常创建输入事件

## 8. 当前阶段定义

到本手册为止，可以认为：

- 大核 `lawrec` 的预览/AI/OSD 链路已打通
- 小核 `lawrec` 的 LVGL 包、buildroot 集成、自启动链已打通

下一阶段重点不再是“怎么把 UI 编进去”，而是：

1. 板端验证小核 UI 是否正常显示和触控
2. 验证小核 UI 与大核 `lawrec` 的 IPC 消息闭环
3. 再决定是否把 `"door_lock"` IPC 服务名整体切换成 `"lawrec"`
