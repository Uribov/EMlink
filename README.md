# EMlink

**EMlink（ESP32-Mesh-Link）** —— 一套开源、低成本的无人机 MAVLink 透传 LoRa Mesh 数传固件。

用几块 ESP32-S3 + LoRa 射频模块，就能给无人机搭一条"会自己接力"的远距离数传链路：飞机端接飞控、地面端接电脑跑 QGroundControl，中间的节点自动转发，无需事先配路由。

> 面向航模 / 无人机长距离遥控遥测场景，目标是把"一套能用的 LoRa Mesh 数传"的成本压到最低。

---

## 目录

- [它能做什么](#它能做什么)
- [硬件要求](#硬件要求)
- [引脚连接](#引脚连接)
- [快速开始](#快速开始)
- [⚠️ 首次烧录必读](#️-首次烧录必读)
- [角色说明](#角色说明)
- [CLI 命令](#cli-命令)
- [射频参数](#射频参数)
- [项目结构](#项目结构)
- [模块清单](#模块清单)
- [技术栈与设计](#技术栈与设计)
- [当前状态](#当前状态)
- [文档](#文档)
- [许可证](#许可证)

---

## 它能做什么

- **MAVLink 透传**：飞控（Pixhawk 等）的 MAVLink 数据双向穿过 LoRa Mesh，地面站 QGroundControl 无感使用。
- **多跳中继**：中间节点自动转发（泛洪 + 去重 + TTL 防环），2+n 拓扑，飞机飞远了靠中继接力。
- **反向路径学习**：节点记住"到某节点该从哪个邻居转发"，单播减少全网广播。
- **大包分片重组**：MAVLink 长消息（如航点、参数）自动分片，收端重组，支持缺件 NACK 补发。
- **帧载荷加密**：AES-128-CCM（mbedTLS），防止链路被篡改/伪造；中继只转发密文，无需持密钥。
- **自适应速率（ADR）**：以 BEACON 为节拍在 SF9/SF7 间切换，兼顾远距离与速率。
- **OTA 升级 + WiFi 网页管理**：CLI 一键进维护模式，手机/电脑连 WiFi 打开网页即可改参数、设密钥、刷固件。
- **统一固件、三种角色**：同一份固件，角色（AIR / GCS / RELAY）通过配置切换。

---

## 硬件要求

| 组成 | 型号 | 说明 |
|---|---|---|
| 主控 | **ESP32-S3 SuperMini** | 双核 240MHz，4MB Flash |
| 射频 | **E220-900M22S** | **LLCC68** 芯片，SPI 接口，22dBm，915MHz（RadioLib 用 `SX1262` 类驱动） |
| 天线 | 900MHz 天线 | SMA / IPEX |
| 飞控侧 | Pixhawk 等（任意 MAVLink 飞控） | 接 AIR 节点的 UART |
| 地面侧 | 电脑 + QGroundControl | 接 GCS 节点的 USB |

> 供电：E220 用 3.3V（**禁止 5V**，上限 3.7V），就近加 10µF + 100nF 去耦。

### 🔴 硬约束：绝对禁止 Type-C 与 4Pin 排针同时插

两路 5V 会**反灌烧板**。二选一使用。

---

## 引脚连接

射频模块（E220-900M22S）↔ ESP32-S3：

| 信号 | E220 脚 | ESP32-S3 GPIO | 方向 |
|---|---|---|---|
| RXEN | 6 | GPIO1 | 输出（收使能，active-high） |
| TXEN | 7 | GPIO2 | 输出（发使能，active-high） |
| DIO2 | 8 | — | **悬空**（正确用法，勿接） |
| DIO1 | 13 | GPIO7 | 输入（中断） |
| BUSY | 14 | GPIO13 | 输入（忙检测） |
| NRST | 15 | GPIO12 | 输出（复位，active-low） |
| MISO | 16 | GPIO11 | SPI |
| MOSI | 17 | GPIO10 | SPI |
| SCK | 18 | GPIO9 | SPI |
| NSS | 19 | GPIO8 | SPI 片选 |
| VCC | 9 | 3.3V | 电源 |
| ANT | 21 | — | 天线 |

飞控 UART：ESP32-S3 **GPIO43/44（UART0）** ↔ 飞控 TELEM 口。
LED：板载 WS2812（状态指示：GCS 绿 / AIR 蓝 / RELAY 青）。

引脚定义全部集中在 `firmware/src/pinmap.h`（板级 `boards/` + 射频级 `radios/`），换板换射频只改这里。

---

## 快速开始

### 1. 装环境

需要 [PlatformIO](https://platformio.org/)（VSCode 扩展或 CLI 均可）。依赖库已全部本地化在 `firmware/lib/`（mavlink2 + radiolib + neopixel），**完全离线编译，不需要联网下载**。

### 2. 编译

```bash
cd firmware
pio run -e esp32s3-supermini-e220-900m22s
```

### 3. 烧录（首次必须先整片擦除）

```bash
cd firmware
pio run -e esp32s3-supermini-e220-900m22s -t erase    # 首次必做！
pio run -e esp32s3-supermini-e220-900m22s -t upload
```

### 4. 看日志 / 敲命令

```bash
pio device monitor          # 115200
```

### 5. 配角色

串口里敲 `role air` / `role gcs` / `role relay`，再 `save`，重启生效。

---

## ⚠️ 首次烧录必读

分区表已从默认的 `default.csv`（单 app + spiffs）改成 **`partitions_ota.csv`（双 app，支持 OTA 回滚）**：

| 分区 | 地址 | 大小 |
|---|---|---|
| nvs | 0x9000 | 0x5000 |
| otadata | 0xe000 | 0x2000 |
| app0 | 0x10000 | 0x180000 |
| app1 | 0x190000 | 0x180000 |
| coredump | 0x310000 | 0x10000 |

**旧分区布局与新表对不上，直接烧会起不来。** 首次必须：

```bash
pio run -t erase     # 整片擦除
pio run -t upload
```

（VSCode 里则是烧录时勾选 "Erase Flash"。）之后正常 OTA 升级不用再擦。

---

## 角色说明

同一份固件，三种角色，上电时从 NVS 读取：

| 角色 | LED | 数据通路 |
|---|---|---|
| **AIR** | 蓝 | 飞控 UART ↔ MAVLink ↔ LoRa |
| **GCS** | 绿 | 电脑 USB(CDC) ↔ MAVLink ↔ LoRa |
| **RELAY** | 青 | 纯中继，只转发，不接 MAVLink 业务 |

典型拓扑：

```
飞控(Pixhawk) ──UART── [AIR 节点] ┄┄LoRa Mesh┄┄ [GCS 节点] ──USB── QGC
                          ↑ 中间可插任意多 RELAY / AIR 节点接力
```

---

## CLI 命令

通过 USB CDC 的第二个虚拟串口（CLI）或串口监视器进入：

| 命令 | 作用 |
|---|---|
| `role air\|gcs\|relay` | 设置角色 |
| `set <key> <val>` | 改配置项（频点/带宽/SF/功率等） |
| `get [key]` | 查看配置（无参数打印全部） |
| `save` | 保存配置并重载 |
| `reset` | 重启 |
| `status` / `stats` | 打印状态 / 统计信息 |
| `crypto <32hex>\|off` | 设置/关闭加密密钥（32 位十六进制） |
| `wifi [off]` | 进入/退出 WiFi 维护模式（OTA + 网页管理） |
| `scan` | 查看最近收包 RSSI/SNR 与邻居表 |
| `ping <node_id>` | 发 PING 测 RTT 与 RSSI |
| `cfg <key> <val>` | 广播参数给全网节点 |
| `help` | 帮助 |

进入 WiFi 维护模式后，手机连上 `EMlink-<节点号>` 热点，浏览器打开 `192.168.4.1` 即可用网页改参数、设密钥、上传固件。

---

## 射频参数

默认值（可在 NVS / CLI 里改）：

| 参数 | 默认值 |
|---|---|
| 频率 | 915.0 MHz |
| 带宽 | 125 kHz |
| 扩频因子 SF | 7（ADR 启用后在 10/7 间自适应） |
| 编码率 CR | 4/5 |
| 同步字 | 0x12（私有） |
| 发射功率 | 20 dBm（模块最大 22dBm） |

---

## 项目结构

```
EMlink/
├── mesh_firmware_architecture.md   # ★ 总架构文档（全局约定的唯一真相源）
├── README.md
├── LICENSE
├── ROADMAP.md                      # 项目尺度 + 待办（按触发条件排队，含"上板后再做"的项）
├── todo.md                         # 板到货后的验证清单（钩子：每条写清看什么、什么算通过）
├── docs/
│   ├── BRINGUP.md                  # 开机操作卡（第一次上板照它走）
│   ├── plans/                      # 方案与拍板记录
│   └── reviews/                    # 审查与修复留档（审查日志 + 修复对照表 + 每项任务书）
├── tools/                          # 开发工具：依赖体检 check_deps.py、批量烧录 flash_batch.sh、boot_app0.bin
└── firmware/
    ├── platformio.ini              # 多方案配置（每个 [env] = 一块板 + 一个射频）
    ├── partitions_ota.csv          # OTA 双分区表
    ├── scripts/build_all.sh        # 批量编译全部方案
    ├── lib/                        # 本地依赖库（mavlink2 + radiolib + neopixel，全离线）
    ├── docs/
    │   ├── CONCEPTS.md             # 基础概念大白话科普（Arduino/PlatformIO/FreeRTOS）
    │   ├── MODULE_INDEX.md         # 模块索引 + 依赖图 + 实现顺序
    │   ├── HARDWARE_VARIANTS.md    # 怎么加新开发板 / 新射频芯片
    │   └── tool.md                 # 环境工具清单
    └── src/
        ├── main.cpp                # 入口（按角色装配启动所有模块）
        ├── pinmap.h                # 引脚入口（boards/ + radios/）
        ├── rtos.h                  # FreeRTOS 对象清单（任务/队列/事件）
        ├── boards/ radios/         # 板级 / 射频级引脚
        ├── drivers/                # 驱动层
        ├── link/                   # 链路层
        ├── net/                    # 网络层
        ├── service/                # 服务层
        └── app/                    # 应用层
```

---

## 模块清单

共 **21 个模块**，分 5 层：

| 层 | 模块 |
|---|---|
| **驱动层** | `radio`（射频收发）、`uart_fc`（飞控 UART）、`usb_cdc`（USB 单口分流：MAVLink + CLI）、`power_detect`（USB 检测） |
| **链路层** | `frame_codec`（组帧/CRC）、`mac`（LBT + ACK 重传）、`duty`（占空比限流）、`adr`（自适应速率） |
| **网络层** | `mesh`（泛洪/去重/TTL）、`route`（反向路径） |
| **服务层** | `mav_router`（MAVLink 解析分发）、`frag`（分片重组）、`addr_map`（sysid↔node）、`nvs_cfg`（配置持久化）、`watchdog`（看门狗+射频自救）、`crypto`（AES-128-CCM）、`ota`（WiFi 网页升级） |
| **应用层** | `role`（数据通路）、`cli`（命令行）、`stats`（统计/LED）、`failsafe_policy`（故障安全） |

每个模块 = 一个 `.h`（接口契约）+ 一个 `.md`（规格说明）+ 一个 `.cpp`（实现）。

---

## 技术栈与设计

- **平台**：PlatformIO + Arduino framework + FreeRTOS
- **射频库**：RadioLib 6.6.0（E220-900M22S 是 LLCC68 芯片，用 **`SX1262` 类**驱动；SF 与带宽必须配对：BW125→SF≤9。依据见 `docs/references/LLCC68_E220-900M22S.md`）
- **双核分工**：
  - **core0**（时间敏感，射频/信道/组网）：`rf_task`、`mac_task`、`mesh_task`
  - **core1**（交互与协议）：`mavlink_task`、`uart_task`、`usb_task`、`cli_task`、`stats_task`、`ota_task`
- **仿 PX4 架构**：模块自包含（任务函数是模块内 `static`，由模块自己的 `xxx_start()` 创建），启动脚本化，启动顺序"下游先就位、上游后跑"。
- **并发安全**：跨核共享状态有明确约定（唯一 seq 源 `mesh_next_seq()`、需要时用 portMUX / mutex 保护），详见总架构文档 §3.5。
- **多硬件方案**：一个 `[env]` = 一块板 + 一个射频模块，用 `-DBOARD_XXX -DRADIO_XXX` 切换，便于扩展到更多 ESP32 系列与射频芯片。

---

## 当前状态

| 项 | 状态 |
|---|---|
| 21 个模块（含 crypto / adr / ota 三个扩展模块） | ✅ 全部实现 + 编译通过 |
| 资源占用 | RAM 17.2% / Flash 55.7%（2026-10-15，A 档修复后） |
| 文档 | 21 个模块 .md 均「编程约束 + 实现架构」双结构 |
| 硬件 bring-up | ⏳ 待上板实测 |

**尚未实现 / 已知限制**：

- `route` 主动路由表（当前用泛洪 + 反向路径学习，节点数 <8 已够用）
- 真正的频率扫描（`scan` 目前只读缓存的 RSSI/邻居表，避免跨核 SPI 冲突）
- 密钥轮换、真随机数（加密已够比赛场景，未做过度设计）

---

## 文档

| 文档 | 内容 |
|---|---|
| [`mesh_firmware_architecture.md`](mesh_firmware_architecture.md) | **总架构**：引脚、分层、任务、帧格式、MAC、分片、路由、QoS、NVS/CLI、统计、扩展模块、未来展望 |
| [`firmware/docs/MODULE_INDEX.md`](firmware/docs/MODULE_INDEX.md) | 模块索引、依赖图、实现顺序 |
| [`firmware/docs/CONCEPTS.md`](firmware/docs/CONCEPTS.md) | 嵌入式基础概念科普（新手向） |
| [`firmware/docs/HARDWARE_VARIANTS.md`](firmware/docs/HARDWARE_VARIANTS.md) | 如何适配新开发板 / 新射频芯片 |
| [`firmware/docs/tool.md`](firmware/docs/tool.md) | 开发环境搭建（装环境 / 编译 / 串口权限 / **校园网走 SSH 推送**） |
| [`docs/references/`](docs/references/LLCC68_E220-900M22S.md) | **平台硬约束档案**：LLCC68 + E220-900M22S 的官方参数、合法组合、驱动匹配规则（含全部出处） |
| [`tools/README.md`](tools/README.md) | 工程工具：批量烧录脚本、分层依赖体检脚本 |
| [`docs/reviews/`](docs/reviews/README.md) | **审查与修复留档**：代码审查日志、修复对照表 `FIX_LOG.md`（条目→提交→验证→遗留）、任务书（已完成的在 `briefs/archive/`） |
| [`docs/BRINGUP.md`](docs/BRINGUP.md) | **开机操作卡**（第一次上板照它走）：备件与接线、串口权限、整片擦除、烧录命令、开机该看什么、`status` 分层怎么读、常见现象对照表、安全红线 |
| [`todo.md`](todo.md) | **板到货后的验证清单**：开箱第一步（同版本同刷 / 整片擦除）、M0 与 A/B 档逐项复验、验收矩阵、已知限制 |
| [`ROADMAP.md`](ROADMAP.md) | **项目尺度与待办**：为什么不过度工程化（个人低成本开源项目）、本批已完成、推后到上板之后的事、按触发条件排队的工程化项 |
| [`docs/plans/`](docs/plans/M2_APP_MID_DRV_PLAN.md) | **方案与拍板记录**：应用-中间-驱动三层契约、体检结果、决策点与结论 |

---

## 许可证

本项目采用 [MIT License](LICENSE)。
