# ArduDeck 使用笔记

## 简介

ArduDeck 是一个开源跨平台地面站，官方描述是「One GCS to rule them all」，同一套界面支持 ArduPilot、PX4、Betaflight、iNav 四种飞控固件。多机是它的主打功能。

对 EMlink 的意义在于：**它能给 iNav 传航点**。支持 MAVLink 的通用地面站做不到这一点（iNav 的 MAVLink 只出不进），而 ArduDeck 内部用 MSP 补上了这块。

本文记录 2026-10-20 在 Linux Mint 上的实际安装、连接 iNav 仿真器、双向传航点的完整过程与踩到的坑。

实测版本：`v0.1.2-rc8`（2026-10-04 发布），Electron 44.4.3、Chrome 152 内核。


## 安装

官方 release 提供两种 Linux 包：

| 文件 | 大小 | 说明 |
|---|---|---|
| `ArduDeck-0.1.2-rc8-linux-x86_64.AppImage` | 206.8 MB | 免安装，推荐 |
| `ArduDeck-0.1.2-rc8-linux-amd64.deb` | 158.7 MB | 需要 sudo |

AppImage 不需要装，`chmod +x` 之后直接执行。系统有 `/dev/fuse` 就能正常挂载运行。

国内网络下 `github.com` 主站 443 端口连不上，release 资产会卡在零字节。走 `api.github.com` 拿签名直链再交给 aria2 分片，16 线程实测约 1.6 MiB/s，206 MB 五分钟下完。


### 启动时的一个坑

第一次按常规方式启动会立刻崩，日志是这样：

```
FATAL:chrome/browser/process_singleton_posix.cc:335] Socket path too long:
/home/sun/.local/share/.../cache/tmp/scoped_dir.../SingletonSocket
```

原因是 Electron 的进程单例 socket 有 108 字节路径上限，而宿主环境把 `TMPDIR` 指到了一个很深的缓存目录。给一个短路径就解决：

```bash
mkdir -p /tmp/ardudeck-tmp
TMPDIR=/tmp/ardudeck-tmp ./ArduDeck-0.1.2-rc8-linux-x86_64.AppImage
```

另外从某些宿主环境（比如 Hermes）拉起 GUI 时，要清掉继承来的 `LD_LIBRARY_PATH`，否则会加载到宿主自带的旧 `libstdc++` 而报 GLIBCXX 版本错误。启动前加 `env -u LD_LIBRARY_PATH` 即可。

启动命令落在 `~/start_ardudeck.sh`。


---

## 界面结构

左侧是一条竖向图标栏，鼠标悬停会显示名称。连接后可用的项会变多（多出 `Calibration`、`CLI Terminal`）。

| 图标位置 | 名称 | 用途 |
|---|---|---|
| 1 | `Telemetry` | 遥测仪表 |
| 2 | `Mission Planning` | 航点规划 |
| 3 | `Parameters` | 飞控参数 |
| 4 | `Calibration` | 传感器校准（连接后出现） |
| 5 | `MAVLink Inspector` | MAVLink 报文查看器 |
| 6 | `Settings` | 设置 |
| 7 | `Firmware` | 刷固件 |
| 8 | `OSD Tool` | OSD 配置 |
| 9 | `Radio HUD` | EdgeTX 遥控器遥测界面 |
| 10 | `SITL Simulator` | 软件在环仿真 |
| 11 | `Cargo` | 任务货物管理 |
| 12 | `CLI Terminal` | 飞控命令行（连接后出现） |
| 13 | `Flight Logs` | 飞行日志 |
| 14 | `Recordings and snapshots` | 录屏与截图 |
| 15 | `Dark theme` | 主题切换 |
| 16 | `Report a Bug` | 生成加密的 bug 报告 |

连接面板（侧栏展开后）分两个标签：`Single vehicle` 和 `Multi-vehicle`。

单机模式下的字段：

- 传输：`SERIAL` / `TCP` / `UDP`
- `Host` + `Port`（TCP/UDP 时有效）
- 固件类型：`iNav` / `ArduPilot` / `PX4`
- 协议：`MAVLink` / `MSP`
- `MAVLink Signing` 开关
- `Connect` / `Disconnect`

底部是 `CONSOLE` 面板，飞控交互的每一步都会在这里留一行带时间戳的日志。**排查问题主要靠它**。


## 连 iNav 仿真器（SITL）

包里已经内置了 iNav 的 SITL 可执行文件（`resources/app.asar` 内的 `sitl/linux/inav_SITL`，1.4 MB），不用自己编译。

操作路径：

1. 左栏展开 `Connection`
2. 点 `SITL Simulator` 那一行里的启动按钮（`Launch virtual flight controller`），固件类型保持 `iNav`
3. 等十几秒，连接面板会出现 `Simulator` 区块，里面有 `Stop SITL` / `Recharge` / `Respawn` 三个按钮
4. 面板下方 `Connected` 区块会显示：

```
Transport   TCP 127.0.0.1:5760
Autopilot   INAV
Vehicle     Multirotor
```

顶栏变成 `DISARMED`，遥测显示 `SAT 0`、`No GPS fix`。这是正常的，仿真器刚起来没有卫星定位，不影响航点的读写。

不接真机就能把整套流程跑完，这一点对验证很有用。


## 传航点（实测）

进 `Mission Planning` 之后，顶部有 `Mission` / `Geofence` / `Rally` 三个标签页，默认在 `Mission`。

**加航点**有两个入口：地图上方的 `Add WP` 按钮（点一次进入加点模式，再点地图落点），或右栏底部的 `Add Waypoint`（直接追加一个）。

**上传下载**都在顶部工具栏，是三个纯图标按钮，靠 `data-tip` 属性分辨：

| `data-tip` | 颜色 | 作用 |
|---|---|---|
| `Download mission from FC` | 蓝 | 从飞控读回航点 |
| `Upload mission to FC` | 绿 | 把航点写进飞控 |
| `Clear mission from FC` | 红 | 清空飞控里的航点 |

右栏航点列表底部还会出现一个带数量的按钮，写着 `Upload 2 WPs` 这种形式，功能与绿色上传图标相同。

2026-10-20 实测结果，CONSOLE 原文：

```
23:40:19.782  Mission saved to EEPROM
23:40:36.462  Downloaded 2 waypoints from FC
```

第一条说明 iNav 收到航点后自己写进了 EEPROM，掉电不丢。第二条是反向读回成功，右栏随即多出一个分组 `From vehicle @ 2026/10/20 23:40:36 — 2 WPs`，与本地那份 `Manual — 2 WPs` 并列。

右栏顶部还显示 `Pre-flight OK`，说明它做了起飞前检查（航点合法性、GPS、模式等），没有拦下来。

iNav 的航点数上限：F1 板 30 个，F3 及更好 60 个。


## 协议：MAVLink 和 MSP 并用

这是最值得记的一条。

连接面板里 `Protocol` 高亮的是 `MAVLink`，已连接时锁死不能改。但 CONSOLE 里持续在刷 MSP 消息：

```
23:40:46.697  [MAIN] [MSP] setRawRc #1501 ch: 1500,1500,1000,1500,1000,1000,1000,1000
```

所以它的实际做法是**两条协议并行**：

- MAVLink：拿遥测（姿态、位置、电压、心跳）
- MSP：航点读写、飞控参数、以及周期性注入 RC 通道

`setRawRc` 那行是它在往飞控灌遥控通道值，八个通道分别 1500/1500/1000/1500/1000/1000/1000/1000，对应横滚、俯仰居中，油门最低，偏航居中，辅助通道全在低位。这属于保持链路活性的手段，不动油门。

这条结论解释了为什么通用地面站做不到：它们的 MAVLink 实现只能收遥测，航点这条路在 iNav 上是堵死的。


## 多机接入

`Multi-vehicle` 标签页默认只显示一句话：

```
Start multi-vehicle
Finds your vehicles automatically. No setup.
```

这是自动发现模式。展开下面的 `Advanced (UDP / TCP / Server sources)` 才是手动配置：

```
CONNECTION SOURCES
ADD A SOURCE
  UDP    TCP    Server
  Port
  Add source
```

也就是说多机是由**多个连接源**堆出来的，每个源独立指定 UDP / TCP / Server。`Server` 是让 ArduDeck 自己监听端口，由外部主动连进来。

三种传输的含义：

- `SERIAL`：本地串口，一个口对应一架
- `TCP` / `UDP`：网络端口，可以开多个
- `Server`：ArduDeck 监听，被动接收

这一点对 EMlink 很关键：MSP 报文里没有地址字段，同一端口上分不出是哪一架飞机，所以多机必须**一机一个口**，靠给每架分配独立端口来区分。


## Swarm 多机仿真（实测）

SITL 页面底部有个 `Run` 区块，里面是 `Single` / `Swarm` 两个模式切换。切到 `Swarm` 后出现这些设置：

| 项 | 说明 |
|---|---|
| `Vehicles` | 仿真几架，默认 4 |
| `Spacing (m)` | 编队间距 |
| `Formation` | `Grid` / `Line` / `Circle` |
| `Copter (Stable)` | 机型 |
| `Wipe EEPROM` | 启动前清飞控 EEPROM |
| `Launch N vehicles` | 启动按钮 |

**端口分配规律**：界面直接写着 `Spawns 4 vehicles on tcp 5760, 5770, 5780, …`，间隔 10。实测启动后四个端口全部在监听：

```
LISTEN 0.0.0.0:5760
LISTEN 0.0.0.0:5770
LISTEN 0.0.0.0:5780
LISTEN 0.0.0.0:5790
```

启动成功后顶栏出现 `Fleet (4)`，进去是机队面板，每架一行：

```
FLEET · 4
  SYS 1   Stabilize   100   QUAD
  SYS 2   Stabilize   100   QUAD
```

每架有独立 `SYSID`。点某一架切换主视图，遥测、航点都作用在当前选中的那架上。

一个和单机不同的地方：**多机 SITL 自带 GPS**，`SAT 10`；单机模式是 `SAT 0`、`No GPS fix`。


### 逐架操作与分组上传

Fleet 面板每架一行 `SYS N`，点一下切到那架。切过去之后，遥测、航点列表、参数都作用在当前选中那架上。

航点有多分组时，顶部那个上传图标会变成禁用，提示改成：

```
Multiple groups: upload one at a time from each group in the list
```

也就是说要回列表里，对每个分组单独点一次上传（每组行尾有独立的上传图标）。这个设计在「本地规划的任务」与「从飞控导回来的任务」并存时会触发，多机时也会。

同理，`Download mission from FC` 未连上目标机时提示为 `Connect to download`，清空按钮提示为 `Connect to clear from FC`。


### AppImage 只读导致 Swarm 起不来

Swarm 模式启动前会对 SITL 可执行文件做 `chmod +x`，而 AppImage 挂载的是只读文件系统，于是抛错：

```
code: 'EROFS', syscall: 'chmod',
path: '.../app.asar.unpacked/resources/sitl/linux/inav_SITL'
```

界面上的表现是永久停在 `Launching…`，`Console Output` 一片空白，只有应用日志里能看到这条 EROFS。单机模式不走这段 chmod，所以不受影响。

解法是解压后运行，得到普通可写目录：

```bash
cd ~/Apps/ardudeck
./ArduDeck-0.1.2-rc8-linux-x86_64.AppImage --appimage-extract   # 得到 squashfs-root/
APPDIR=$PWD/squashfs-root TMPDIR=/tmp/ardudeck-tmp \
  ./squashfs-root/AppRun --remote-debugging-port=9222
```

解压出 755 个文件、约 575 MB。`inav_SITL` 在 `squashfs-root/resources/app.asar.unpacked/resources/sitl/linux/`，解压后可以直接 chmod。

**`APPDIR` 必须显式给**。`AppRun` 脚本在没有 `APPDIR` 时用 `$1` 反推安装目录（`while [[ ! -e "$path/$1" ]]`），而我们传的第一个参数是 `--remote-debugging-port=9222`，永远匹配不到文件，路径一路退到空字符串，最后报 `/@ardudeckdesktop: 没有那个文件或目录`。

启动脚本落在 `~/start_ardudeck_extracted.sh`，检查脚本 `~/check_swarm.sh`。


## 无人值守操作（调试端口）

ArduDeck 是 Electron 应用，加上 `--remote-debugging-port=9222` 启动后可以用 Chrome DevTools Protocol 从外部读页面、点按钮，不必靠截图猜坐标。

```bash
TMPDIR=/tmp/ardudeck-tmp ./ArduDeck-*.AppImage --remote-debugging-port=9222
```

启动后 `curl http://127.0.0.1:9222/json/list` 能看到页面目标。实测可用的几个查法：

```js
// 列出所有按钮的文本与坐标
[...document.querySelectorAll('button,[role="button"]')].map(...)

// 按 data-tip 找工具栏图标按钮
[...document.querySelectorAll('button')].filter(b => b.getAttribute('data-tip'))

// 按坐标反查某点是什么元素
document.elementFromPoint(x, y).closest('button')
```

注意几点：

- 工具栏图标的提示文字挂在 **`data-tip`** 上，不是 `data-tooltip`
- `Ready to upload` 只是状态文字，不是按钮
- 左侧导航图标除 `title` 外没有文本，靠 `title` 属性识别
- 界面会随操作换布局，坐标不固定，每次操作前重新取一遍

脚本在 `~/cdp.py`（执行 JS、列按钮、按文本点击）和 `~/probe_toolbar.py`（按坐标反查元素）。


## 踩过的坑

| 现象 | 原因 | 解法 |
|---|---|---|
| 启动即崩，`Socket path too long` | Electron 单例 socket 路径超 108 字节 | 设短 `TMPDIR` |
| 报 `GLIBCXX_3.4.31 not found` | 宿主注入的旧 `libstdc++` | 启动器层 `env -u LD_LIBRARY_PATH` |
| `data-tooltip` 查不到按钮 | 属性名实际是 `data-tip` | 改属性名 |
| 点 `Advanced` 没反应 | 它是 `<details><summary>`，点文本容器无效 | 用 `elementFromPoint` 找真实 `summary` 再点 |
| 脚本在新 shell 里报 `No module named websockets` | 新 shell 的 `python3` 指向 conda 版 | 用 `/usr/bin/python3` |
| 截图抓错窗口 | `gnome-screenshot -w` 抓的是当前活动窗口 | 先 `wmctrl -a` 置前，或全屏截再裁 |
| 窗口几何突然变 | 窗口被最大化过 | 每次操作前 `wmctrl -lG` 重新取坐标 |
| Swarm 一直停在 `Launching…` | AppImage 挂载只读，`chmod` 报 `EROFS` | 用 `--appimage-extract` 解压后运行 |
| 解压版报 `/@ardudeckdesktop: 没有那个文件或目录` | `AppRun` 用 `$1` 反推安装目录，被启动参数骗到空路径 | 显式传 `APPDIR=<squashfs-root 绝对路径>` |


## 对 EMlink 的意义

原先设计的方案是「在 EMlink 固件里做 MAVLink 到 MSP 的转译层」。实测表明这一步**不需要 EMlink 做**：ArduDeck 已经在电脑侧完成了，EMlink 只要原样搬运字节流。

`frame_codec` 的 `proto` 字段本来就留了 `0=MAVLink / 1=CRSF / 2=MSP / 3=自定义`，架构上早就留了口子，正好用得上。

真正要解决的是多机侧的分流：MSP 没有地址字段，所以 EMlink 的 GCS 端需要按每架飞机的 node_id 开出独立端口，每个端口喂给 ArduDeck 一个连接源。MAVLink 那一侧本身带 sysid，单端口多机理论可行，但航点必须走 MSP，所以最终还是得按机分口。

---

## 待验证

- 真机 iNav 飞控（非 SITL）上的航点读写，SITL 通过不代表实机通过
- EMlink GCS 端如何按 node_id 分流到多个 TCP 端口，需要固件/上位机配合开接口
- Swarm 四架同时在线的状态下，逐架选中并各自上传航点是否互不干扰
- Betaflight 侧是否也走 MSP 航点，其 2026.6 的 autopilot 官方标注仅仿真可用
