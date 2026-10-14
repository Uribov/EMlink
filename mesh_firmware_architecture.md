# 无人机 915MHz LoRa Mesh 数传 · 最终固件架构

> 硬件：ESP32-S3 SuperMini + E220-900M22S（LLCC68 / SX1268 类 SPI 裸射频模块）
> 统一硬件、统一固件，角色（AIR / GCS / RELAY）上电由 NVS + USB CLI 配置
> 平台：PlatformIO + Arduino + FreeRTOS + RadioLib
> 硬约束：**绝对禁止 Type-C 与 4Pin 排针同时插**（双路 5V 反灌烧板）

---

## 0. 硬件接口与引脚定义（已打板，全篇基准）

### 0.1 模块定性（已核实，非猜测）

| 项 | 结论 |
|---|---|
| 接口 | **SPI（MISO/MOSI/SCK/NSS）+ 控制脚**，**不是 UART 透传** |
| 核心芯片 | LLCC68（Semtech，与 SX1268 同族，RadioLib 用 `SX1268` 驱动） |
| VCC | 1.8~3.7V，本设计 3.3V（AMS1117）✓ |
| 时钟 | 无 TCXO，晶振版（RadioLib 用默认 XTAL 配置）。已确认可接受：SF7/BW125 对频偏极宽容（容差约 ±25% 带宽 ≈ ±31kHz），视距 + 中继有充足链路余量，无需用 TCXO 换极端灵敏度 |
| 射频开关 | 外部开关，**RXEN / TXEN 控制，active-high** |
| DIO2 | **悬空 = 正确用法**（不用时必须悬空；且 DIO2 只能接 TXEN，不可接 MCU IO 当开关） |
| DIO3 | 本封装未引出，无需连接 |

### 0.2 引脚表（以打板原理图为准，固定不变）

| 射频信号 | E220 脚 | ESP32-S3 GPIO | 方向（对 MCU） | 用途 / 代码 |
|---|---|---|---|---|
| RXEN | 6 | GPIO1 | 输出 | 收使能，active-high，接 `setRfSwitchPins` |
| TXEN | 7 | GPIO2 | 输出 | 发使能，active-high |
| DIO2 | 8 | — | 悬空 | 不接 ✓ |
| VCC | 9 | 3.3V(AMS1117) | 电源 | 就近加 10µF+100nF 去耦 |
| DIO1 | 13 | GPIO7 | 输入(IRQ) | 收发完成中断 |
| BUSY | 14 | GPIO13 | 输入 | 忙检测 |
| NRST | 15 | GPIO12 | 输出 | 复位，active-low |
| MISO | 16 | GPIO11 | 输入 | SPI |
| MOSI | 17 | GPIO10 | 输出 | SPI |
| SCK | 18 | GPIO9 | 输出 | SPI |
| NSS | 19 | GPIO8 | 输出 | SPI 片选 |
| ANT | 21 | — | 天线 | 焊接 SMA/IPEX |
| GND | 1–5,10–12,20,22 | GND | 地 | 全接地，与 MCU 共地 |

### 0.3 接口层结论（照此写驱动）

```cpp
// RadioLib 实例化（LLCC68 == SX1268），引脚宏来自 pinmap.h（radios/e220-900m22s.h）
SX1268 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY);
radio.setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);   // 用 RXEN/TXEN 切收发
// 禁止调用 setDio2AsRfSwitchCtrl() —— DIO2 已悬空
radio.begin(freq, bw, sf, cr, syncWord, pwr);
```

### 0.4 引脚核对结论（E220 该连的已全部连齐 ✓）

| E220 脚 | 信号 | S3 GPIO | 状态 |
|---|---|---|---|
| 1–5,10–12,20,22 | GND | GND | ✓ 共地 |
| 9 | VCC | 3.3V | ✓（上限 3.7V，禁接 5V） |
| 16/17/18/19 | MISO/MOSI/SCK/NSS | 11/10/9/8 | ✓ SPI 全连 |
| 13 | DIO1 | GPIO7 | ✓ 中断 |
| 14 | BUSY | GPIO13 | ✓ 忙检测 |
| 15 | NRST | GPIO12 | ✓ 复位 |
| 6/7 | RXEN/TXEN | GPIO1/2 | ✓ 射频开关 |
| 8 | DIO2 | 悬空 | ✓（正确） |
| 21 | ANT | 天线 | ✓ |

- **TX→RX / RX→TX 已澄清**：这是 S3 的 UART0（GPIO43/44），接飞控（Pixhawk），与 E220 无关。
- **GP13 = BUSY 占用 FSPI 默认 MISO**：故 SPI 走 GPIO 矩阵重映射（NSS=8/SCK=9/MOSI=10/MISO=11），已固化在 `pinmap.h`。
- **RXEN/TXEN 电平**：active-high，RadioLib `setRfSwitchPins` 默认匹配；P0 若方向反，量电平回填。
- **供电**：22dBm 峰值 ~119mA + ESP32 瞬态，AMS1117 就近 10µF+100nF 去耦。

---

## 1. 系统全景

```
飞控(Pixhawk) ──UART── [AIR 节点] ┄┄LoRa Mesh┄┄ [GCS 节点] ──USB── QGC
                        ↑ 中间可有其它 AIR 节点接力（2+n / n 终端）
```

统一固件，三角色，角色存 NVS：
- **AIR**：接飞控 UART，MAVLink ↔ LoRa 双向翻译。
- **GCS**：接电脑 USB(CDC)，MAVLink ↔ LoRa 双向翻译。
- **RELAY**：纯 Mesh 中继，无 MAVLink 业务，只转发。

---

## 2. 分层与模块职责

```
┌─ 应用层   role / cli / stats / failsafe_policy（角色、命令行、统计、故障安全策略）
├─ 服务层   mav_router / frag / nvs_cfg / addr_map / watchdog / crypto* / ota*
├─ 网络层   mesh / route（泛洪转发、去重、防环、反向路径、路由表预留）
├─ 链路层   frame_codec / mac / duty / adr*（组帧拆帧、LBT、ACK 重传、占空比）
└─ 驱动层   radio / uart_fc / usb_cdc / power_detect
           （* 为预留模块，见 §13）
```

| 层 | 模块 | 职责 | 核 |
|---|---|---|---|
| 驱动 | `radio` | 射频抽象接口：收发、CAD 侦听、RSSI/SNR、复位（SX126x 等按宏实现） | 0 |
| 驱动 | `uart_fc` | 与飞控的 UART（DMA 环形缓冲） | 1 |
| 驱动 | `usb_cdc` | 双虚拟串口：CDC0=MAVLink，CDC1=CLI | 1 |
| 驱动 | `power_detect` | USB 插入/拔除检测、电源状态机输入 | 1 |
| 链路 | `frame_codec` | 组帧/拆帧、CRC、序号管理 | 0 |
| 链路 | `mac` | LBT（CAD）、ACK 超时重传、退避 | 0 |
| 链路 | `duty` | 占空比限流（每节点每秒空中时间上限） | 0 |
| 链路 | `adr` | 自适应速率（**预留**，§13.3） | 0 |
| 网络 | `mesh` | 泛洪转发、去重、TTL 防环 | 0 |
| 网络 | `route` | 反向路径学习、路由表（预留） | 0 |
| 服务 | `mav_router` | MAVLink 按 sysid 分发、心跳优先中转 | 1 |
| 服务 | `frag` | MAVLink 大包分片/重组、缺件 NACK | 1 |
| 服务 | `addr_map` | sysid↔node_id 对照表（从 HEARTBEAT 学习） | 1 |
| 服务 | `nvs_cfg` | NVS 读写 | 1 |
| 服务 | `watchdog` | 软/硬看门狗喂狗、射频卡死自救 | 1 |
| 服务 | `crypto` | 帧载荷加密（**预留**，§13.1） | 1 |
| 服务 | `ota` | OTA 固件升级（**预留**，§13.2） | 1 |
| 应用 | `role`/`cli`/`stats`/`failsafe_policy` | 角色逻辑/命令行/统计/故障安全策略 | 1 |

---

## 3. FreeRTOS 任务、核、优先级、队列、事件

### 3.1 任务表

| 任务 | 核 | 优先级 | 职责 |
|---|---|---|---|
| `rf_task` | 0 | 22 | DIO1 中断→任务通知→读 RadioLib、把原始帧投 `q_rf_rx` |
| `mac_task` | 0 | 20 | 发信排队、LBT、ACK 超时重传、退避 |
| `mesh_task` | 0 | 18 | 转发判定、去重、TTL 减一、回投 `q_mac_tx` |
| `mavlink_task` | 1 | 12 | MAVLink 解析、按 sysid 分发、心跳优先、分片触发 |
| `uart_task` | 1 | 10 | 飞控 UART 收发（DMA） |
| `usb_task` | 1 | 10 | USB CDC 收发（MAVLink + CLI 分路） |
| `cli_task` | 1 | 8 | 命令行交互 |
| `stats_task` | 1 | 4 | 1s 周期统计、LED、喂狗、失联判定 |

> 核0 = 射频/信道/组网（时间敏感，独占不被打断）；核1 = 交互与协议（节奏慢）。
> **核心绑定**：本项目用 LoRa 不开 WiFi/BT，core0 空闲给射频；将来加 WiFi（固件 OTA，§13.2）时 WiFi 栈占 core0，射频任务需重估核心。
> **任务函数归属**：8 个任务函数是各模块 `.cpp` 内 `static` 实现，由模块的 `xxx_start()` 创建（仿 PX4 模块自包含），`main.cpp` 的 `setup()` 按角色依次调用。

### 3.2 队列（解耦，任务间不直接抢数据）

| 队列 | 方向 | 内容 |
|---|---|---|
| `q_rf_rx` | rf_task → mesh_task | 收到的原始帧 |
| `q_mac_tx` | mesh/mavlink → mac_task | 待发帧（带 QoS 优先级） |
| `q_uart_rx` | uart_task → mavlink_task | 飞控上行 MAVLink 字节流 |
| `q_usb_rx` | usb_task → mavlink_task | 地面站下行 MAVLink 字节流 |
| `q_uart_tx` | mavlink_task → uart_task | 下发飞控 MAVLink |
| `q_usb_tx` | mavlink_task → usb_task | 上送地面站 MAVLink |
| `q_frag` | mavlink_task → frag | 待重组分片 |

> `q_mac_tx` 为**优先级队列**：QOS2 > QOS1 > QOS0，同类 FIFO。

### 3.3 事件（信号旗）

| 事件 | 触发 | 订阅者 |
|---|---|---|
| `EV_USB_PLUG` | power_detect | failsafe_policy、cli_task |
| `EV_USB_UNPLUG` | power_detect | failsafe_policy |
| `EV_LINK_LOST` | stats_task(3s 无上行) | failsafe_policy、LED |
| `EV_LINK_RESTORED` | mac_task(ACK 恢复) | failsafe_policy、LED |
| `EV_RF_FAULT` | radio(连续无响应) | watchdog |
| `EV_CFG_CHANGED` | cli_task(save) | nvs_cfg→通知各模块重载 |

> 以上任务/队列/事件的句柄、优先级、事件位宏统一定义在 `firmware/src/rtos.h`（唯一真相源），各模块 `#include "rtos.h"` 引用，不重复创建。
> **队列所有权**：队列只由 `rtos_comms_init()` 统一创建（幂等，必须最先调用），任何模块 `start` 内禁止再建；句柄注释标注「生产方 → 消费方」。

### 3.4 启动依赖顺序（下游先就位，上游后跑，防开机首包丢）

```
rtos_comms_init()          # 建队列/事件组（最先，幂等）
→ radio_start()            # 射频底层
→ mac_start()              # 信道接入
→ mesh_start()             # 转发
→ mav_router_start()       # MAVLink 路由（被动收队列，先就位）
→ uart_fc_start()          # 飞控串口（上游，后跑）
→ usb_cdc_start()          # USB（上游，后跑）
→ cli_start()              # 命令行
→ stats_start()            # 统计/LED/喂狗
```

> **栈大小**：先启用 `configCHECK_FOR_STACK_OVERFLOW` 跑一轮，按实测栈使用反推各任务栈大小，再回填 §3.1 的栈值。

---

## 4. Mesh 帧格式、地址、去重、TTL、路由

### 4.1 帧格式（MTU = 200B）

| 偏移 | 字段 | 字节 | 说明 |
|---|---|---|---|
| 0 | `ver:type` | 1 | 高4b 版本=1；低4b 类型 0=DATA 1=ACK 2=NACK 3=BEACON 4=PING 5=ROUTE(预留) |
| 1 | `flags` | 1 | `qos[7:6]`、`ack_req[5]`、`frag[4]`、`proto[3:2]`、`reserved[1:0]` |
| 2 | `src_id` | 1 | 源节点地址 1–254 |
| 3 | `dst_id` | 1 | 目的节点地址；`0x00`=广播 |
| 4–5 | `seq` | 2 | 源节点序号（LE，回绕） |
| 6 | `ttl` | 1 | 剩余跳数，转发减一，0 丢弃 |
| 7 | `orig_sysid` | 1 | 源 MAVLink sysid（发送者，供 addr_map 学习/分片重组键） |
| 8 | `net_id` | 1 | 网络 ID（多网共存，0=默认网） |
| 9–12 | `frag_hdr` | 4 | 分片头（§6），非分片帧填 0 |
| 13–192 | `payload` | ≤180 | MAVLink 消息或分片数据 |
| 193–194 | `crc16` | 2 | 帧级 CRC16（端到端完整性，可选但建议开） |

> `proto`（flags[3:2]）payload 协议类型：0=MAVLink、1=CRSF、2=MSP、3=自定义。
> `net_id` 默认 0；非 0 时只收同网帧，实现多网共存。

> 空中由 LoRa 硬件 CRC（CR=4/5）逐跳保护；帧级 CRC16 用于 mesh 逐跳重编码后的端到端校验。

### 4.2 地址方案（两套体系，各管各的）

- **node_id（节点地址）**：无线网内部节点号 1–254，唯一，存 NVS。
- **sysid（系统标识）**：MAVLink 业务号，飞控=1、地面站=255，多机多系统标识。
- 二者通过 `addr_map` 关联（§7），互不污染。

### 4.3 去重

- 键 = `(src_id, seq)`。
- 每源一个 256 槽滑动窗口（bitmap + 上次序号），序号回绕处理。
- 命中即**丢弃，不处理不转发**。
- 槽位 30s 未更新自动作废。

### 4.4 TTL

- 默认 5（最多 4 跳）。每转发 `ttl--`，到 0 丢并计 `dropped_ttl`。

### 4.5 路由（三级）

1. **泛洪（默认）**：广播帧人人转发一次，简单可靠、抗节点移动，小网最优。
2. **反向路径**：收到帧时记 `src_id ← 上一跳邻居`，回信走单播原路，省一半空口。
3. **路由表（预留，未启用）**：帧类型 5 已留位，节点数 >8 再上。

---

## 5. MAC 与信道接入

- **LBT（先听后说）**：发前 CAD 侦听 1 次；忙→随机退避 5–20ms 重试，最多 3 次；3 次仍忙→**硬发**（控制指令不可无限憋）。
- **半双工**：RXEN/TXEN 切收发，发完立刻切回收听。
- **ACK/重传**：`ack_req=1` 的帧，收方回 ACK；发方 300ms 未收到→重发，最多 3 次，退避 100→300→700ms。
- **BEACON**：GCS 每 1s 广播，节点据此评估邻居链路质量（RSSI/SNR）。
- **占空比**：每节点每秒空中发射时间上限（200B@SF7/BW125 ≈ 60ms 空中，上限设 ~10%），超限排队稍等——守法规、让空口。

---

## 6. MAVLink 分片与重组

- MAVLink 2 最大帧 267B（无签名），单帧 payload 仅 180B，超长消息拆片。
- **分片头（4B）**：`msg_id(1) + frag_index(1) + frag_total(1) + frag_seq(1)`。
- 重组键 = `(orig_sysid, frag_seq)`；2s 未拼齐→整组作废，计 `reasm_timeout`。
- 缺件：收方回 NACK（`frag_index` 缺失位图），发方**只补缺件**，不重发全部。
- 遥测/心跳/单航点项（≤38B）**不分片**，只有大消息才触发。

---

## 7. 多 AIR / GCS 与 sysid 路由

- **GCS→多机**：GCS 广播，各 AIR 节点看 MAVLink 消息 payload 里的 `target_sysid`，匹配自己飞控才送上 UART，否则丢弃。
- **AIR→GCS**：反向路径单播回 GCS；GCS 合并各机数据上 USB，QGC 靠 sysid 自行区分。
- **addr_map**：从 HEARTBEAT 学习 `sysid ↔ node_id`，兼作地面站链路质量显示来源。
- **多 GCS**：各自监听，控制权交飞控/上位机仲裁（MAVLink 自带），固件只透明搬运。

---

## 8. QoS 与故障安全（安全优先）

### 8.1 QoS 三级

| 档 | 机制 | 用途 |
|---|---|---|
| QOS0 | 不确认，丢就丢 | 遥测流（下帧即补） |
| QOS1 | ACK + 重传×3 | 命令、参数读写、单航点 |
| QOS2 | 逐件确认 + 缺件补发 | 整段航点上传、关键控制 |

排队优先级：**控制 > 参数/航点 > 遥测**。

### 8.2 故障安全（固件绝不越权替飞控做决定）

1. **心跳优先中转**：GCS HEARTBEAT 进 `q_mac_tx` 最高优先级，绝不在本节点滞留。返航判定是**飞控自己**数心跳超时，固件只保证心跳不误时。
2. **链路看门狗**：AIR 侧 3s 无上行→`EV_LINK_LOST`，告警、停止无效转发，等飞控超时接管。
3. **射频卡死自救**：连续 N 次无响应→软复位射频芯片（NRST），再不行重启整机；计 `rf_reinit`。
4. **双看门狗**：任务级 SW WDT + HW WDT（RTC/看门狗定时器兜底）。
5. **禁止双供电（硬约束）**——供电电路（已定案）与电源/接口状态机：

   供电电路：
   ```
   C 口 VBUS ────────────────→ 5V 引脚（S3 供电入口，无隔离，不能加二极管）
   4Pin 5V ── SS34 ─────────→ 5V 引脚（单向，防倒灌回飞控/电池）
   5V 引脚 ── PTC 0.5A ─────→ AMS1117 → 3.3V → E220
   ```
   硬件防护：SS34（4Pin 侧防倒灌）+ PTC 0.5A（AMS1117 前防过流）。C 口侧不能串二极管（会挡住 4Pin 反向供电，S3 吃不到电）。剩余靠「禁双插约定 + 固件状态机」兜底。

   状态机判据：**「角色 + USB 是否枚举」**，不测 5V 电压（C 口/4Pin 供电时 5V 引脚都有电，测不出来源）。USB 枚举 = 与主机握手成功 = USB 数据通路可用。

   ```
   上电读 role ──┬─ 未设/配置模式 ──→ 态A「仅USB配置」：USB=CLI，射频停
                 ├─ role=AIR ───────→ 态B「AIR 仅串口」：UART↔LoRa 数据通路
                 └─ role=GCS ───────→ 态C「GCS USB 跑 MAVLink」：USB=MAVLink 数据通路

   飞行中(B态) 若 USB 枚举(插上电脑) → 只弹 CLI，绝不切换数据通路（防数据流被 USB 抢走）
   ```

---

## 9. NVS 配置项与 CLI

| 键 | 默认 | 说明 |
|---|---|---|
| `role` | 未设→强制进 CLI | `air` / `gcs` / `relay` |
| `node_id` | 自动分配 | 节点地址 1–254 |
| `sysid` | AIR=飞控号，GCS=255 | 系统标识 |
| `freq` | 915.0 | 工作频率 MHz |
| `sf` / `bw` / `cr` | 7 / 125 / 4 | 扩频因子/带宽/编码率 |
| `power` | 20 | dBm（默认留余量） |
| `syncword` | 私有 | 组网暗号 |
| `uart_baud` | 115200 | 飞控串口波特率 |
| `ttl` | 5 | 最大跳数 |
| `ack_timeout_ms` | 300 | ACK 耐心 |
| `retries` | 3 | 重发次数 |
| `beacon_s` | 1 | 信标周期 |
| `failsafe_s` | 3 | 失联判定 |
| `name` | — | 节点名 |

CLI（CDC1）常用命令：

```
role gcs|air|relay      set <键> <值>      get
save                    reset              status
stats                   scan               ping <node_id>
mavlink monitor on      （旁路观察 MAVLink）
```

---

## 10. 调试与统计

### 10.1 统计（`stats`，1s 刷新）

| 类 | 指标 |
|---|---|
| 链路 | `tx_frames` `rx_frames` `ack_ok` `ack_miss` `retries` |
| 网络 | `fwd` `dropped_ttl` `dropped_dup` `reasm_timeout` |
| 空口 | `rssi` `snr` `link_quality%` |
| 系统 | `heap_free` `wdt_resets` `rf_reinit` |
| 路由 | 邻居表（node_id、rssi、last_seen） |

### 10.2 日志与状态

- 四级：`ERR/WARN/INFO/DBG`，按需开关。
- 日志出口：**走 USB 或专用串口，绝不占飞控 UART**。
- LED 状态：WS2818 RGB@GPIO48，颜色=身份（角色）、节奏=健康（链路），完整映射表见 `firmware/src/app/led_states.md`。

---

## 11. 分步实施顺序与验收标准

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0 点亮** | pinmap.h 核对、USB、NVS、角色检测、SPI 射频、UART 全通，两板对发对收 | 10m 内互发互收，RSSI 入日志 |
| **P1 链路层** | 帧编解码、ACK/重传、LBT | 2 节点 500m 视距 **0 丢包** |
| **P2 组网** | 泛洪、去重、TTL、反向路径 | 3 节点端到端送达、无死循环、包数对得上 |
| **P3 业务** | MAVLink 翻译、分片、sysid 路由、心跳中转 | 同时看到 2 机；能读参数/传航点/收遥测 |
| **P4 安全** | QoS 优先级、看门狗、断链行为 | 掐上行→飞控 3s 内进返航；恢复→自动重连 |
| **P5 外场** | CLI 完善、500m/120m 实测、抗干扰 | 见下方三硬指标 |

**最终验收三硬指标**：
1. 500m 视距投递率 **≥99%**（SF7）；
2. 地面站↔飞机控制指令单程 **<1s**（含重传）；
3. 断链到飞控触发返航 **<3s**。

---

## 12. 风险清单

1. **频段合规**：中国 LoRa 主流 470–510MHz，915MHz 属 ISM 但需核当地频谱政策；默认功率 20dBm 留余量。
2. **内存**：去重表（16 源 × 32B 位图）+ 重组缓冲（4 并发 × 267B）预估仅几 KB，SRAM 即够，PSRAM 作额外余量；4MB Flash 足够。
3. **同频多机**：LBT 只缓解不根除，>8 节点再考虑跳频。
4. **原理图红灯**：§0.4 的 UART/SPI 疑点、GP13/BUSY 冲突、RXEN/TXEN 电平，P0 第一件事核对。
5. **双供电反灌**：固件状态机 + 板级防反灌，两手都要。

---

## 13. 预留扩展点（Roadmap）

> 以下功能已规划但未实现，作为后续演进方向。帧格式/接口已预留钩子，实现时读本节即可，无需推倒重来。

### 13.1 加密（crypto）
- 帧头 `flags` 余位[1:0] 再留 1 位「加密标志」
- payload 前留 nonce/IV（AES-CCM），帧头不加密（要能路由）
- ESP32-S3 硬件 AES 加速；`seq` 字段复用做防重放
- 位置：`service/crypto`

### 13.2 OTA 固件升级（WiFi，ota）
- 落地维护时连 WiFi，用 `esp_ota` 快速升级固件（LoRa 速率太低不适合传固件，只做参数 OTA，见 §13.4）
- `partition` 表改用 OTA 双分区（ota_0/ota_1 + otadata 回滚）
- 4MB Flash 双 OTA（1.5M×2）+ otadata + NVS 够用，**不加大的 spiffs**
- 流程：连 WiFi → 下载固件 → 校验 → 写 ota_1 → 重启切分区
- 位置：`service/ota`（依赖 WiFi 栈，占 core0，届时重估核心分配）

### 13.3 自适应速率（adr）
- 根据 RSSI/SNR 动态调 SF/BW：近距离降 SF 提速，远距离升 SF 保链路
- `radio.h` 已有 `radio_lastRssi()/radio_lastSnr()` 接口
- 位置：`link/adr`

### 13.4 远程参数同步（预留，无独立模块）
- 帧类型预留「配置帧」，GCS 广播配置给全网
- 实现时在 `frame_codec.h` 加 `FRAME_CFG` 类型
