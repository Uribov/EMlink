# usb_cdc —— 单口分流（一个 USB 虚拟串口同时跑 MAVLink 与 CLI）

## 职责

ESP32-S3 原生 USB-OTG 只开一个 CDC（`Serial` = USB 虚拟串口）。这一个口上同时走两种数据，由 `usb_task` 在字节层分流：

- `0xFD`（MAVLink v2）/ `0xFE`（v1）开头 → 按帧收完整帧 → `q_usb_rx` → `mav_router`（地面站）
- 其余字节 → CLI 文本环形缓冲 → `usb_cli_read()` → `cli` 模块

分流的判据只看首字节，不需要解析内容，所以两种数据混在同一条流里也不会互相干扰。


## 依赖

- ESP32-S3 USB 外设（Arduino 框架内置；`platformio.ini` 已配 `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`）
- `rtos`（队列 `q_usb_rx`/`q_usb_tx`、任务栈/优先级宏）

依赖只有 Arduino 内置的 USB 外设和一个队列层。


## 对外接口

见 `usb_cdc.h`。要点：`usb_cdc_init / usb_cdc_start / usb_mav_write / usb_cli_available / usb_cli_read / usb_cli_write` + `usb_cdc_drop_count()`（下行队列满丢弃，P1-10）+ `usb_cli_drop_count()`（CLI 环形缓冲满丢弃）。

> `usb_mav_available/usb_mav_read` 仅为兼容保留，已不作为接收路径（接收统一由 `usb_task` 接管）。

收发各有明确入口，其中接收路径统一走 `usb_task`。


## 验收（P0）

- 插 USB 出现一个 COM 口；能收到开机日志，能敲 `help` 有回显，同时地面站能连上收遥测

一条就验全了：单口、双向、两种数据并存。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

只有一个虚拟串口，MAVLink 帧和 CLI 文本在字节层分开：进帧头就按帧收，否则当文本喂给 CLI。


### 只有一个 CDC

早期设计假设「`Serial`=CDC0(MAVLink)、`Serial1`=CDC1(CLI)」。这个假设不成立：

本工程从未配置 TinyUSB 复合描述符，而 `-DARDUINO_USB_MODE=1` 下 USB 只有一个 CDC；ESP32-S3 上 `Serial1` 是 UART1 硬件串口（能编译过 ≠ 存在第二个 CDC），代码也从未 `Serial1.begin()`、引脚没接任何东西

→ 结果是 CLI 输出进黑洞、输入永远为 0，而「设角色的唯一入口就是 CLI」→ 首次上电直接卡死。

2026-10-15 按审查日志 P1-6 选项①（单口文本分流）修掉。

判断 USB 有几个 CDC，要看 `ARDUINO_USB_MODE` 的配置，不能凭 `Serial1` 能编译过就认定它存在。


### 串口映射

| 逻辑口 | Arduino 对象 | 用途 |
|---|---|---|
| USB 虚拟串口 | `Serial` | MAVLink（地面站）+ CLI 共用，字节层分流 |
| 飞控 UART | `Serial0`（UART0，GPIO43/44） | `uart_fc` 用（仅 AIR 角色） |

两个串口对象对应两条完全独立的通路，中间没有交叉。


### 数据流

```
地面站 ──> Serial ──[usb_task 字节分流：0xFD/0xFE 按帧收]──> q_usb_rx ──> mav_router
地面站 <── Serial <── q_usb_tx <── mav_router            （写前取 g_usb_tx_lock）
键盘  ──> Serial ──[usb_task 分流：非帧字节]──> CLI 环形缓冲(256B) ──> cli_task
CLI   <── Serial <── cli_print（写前取同一把 g_usb_tx_lock）
```

上行按字节分流到两条队列，下行共用 `Serial` 但靠同一把锁避免互相插入。


### 关键实现细节

1. 发送方向用互斥量（`g_usb_tx_lock`）而不是队列：CLI 需要立刻回显（队列做不到同步）；MAVLink 侧本就由 `usb_task` 独占写，加锁只为防 CLI 把文本插进 MAVLink 帧中间。
2. `usb_task` 是 `Serial` 的唯一读者：按块读入后逐字节喂状态机；`cli_task` 只从环形缓冲取字节（所以 `cli.cpp` 一行都不用改，逐字节读的语义不变）。
3. 帧长公式（状态机用它决定收多少字节）：
   - v2：`0xFD|len|incompat|compat|seq|sysid|compid|msgid[3]|payload[len]|crc[2]` → 总长 = 10 + len + 2
   - v1：`0xFE|len|seq|sysid|compid|msgid|payload[len]|crc[2]` → 总长 = 6 + len + 2
4. 卡帧保护（500ms）：收帧中超过 500ms 无新字节 → 复位状态机（丢半截帧），并把当前字节当作新起点处理。否则一次误判（例如有人在 CLI 里粘了含 `0xFD` 的二进制）会让接收方向永久卡死。
5. 投递分片：`queue_item_t.data[]` 只有 `FRAME_MTU`(200) 字节，而 MAVLink v2 帧可达 267 → `usb_frame_deliver()` 按 `FRAME_MTU` 分片投 `q_usb_rx`（`mav_router.feed_usb` 是字节流消费者，分片无影响）。
6. `main.cpp` 无条件启动 USB 任务：CLI 就在这个口上，三种角色（含 `ROLE_UNSET` 首次上电）都要起，否则「没命令可用 → 角色设不了 → 自锁」。
7. 帧内字节不会被误判：状态机在 `RECV` 阶段只按计数盲存，不解释内容 —— 所以 payload 里出现 `0x0A`/`0xFD` 既不会被当文本也不会触发新帧。

七条里 1~2 是并发与所有权，3~5 是状态机与分片，6~7 是启动顺序和帧内字节的安全性。


### 已知限制

- CLI 里粘贴含 `0xFD/0xFE` 的二进制可能被误判成帧头 → 靠 500ms 超时自恢复
- 与地面站同口并用时：CLI 文本会混进下行流（地面站忽略非 MAVLink 字节）；地面站的二进制不会被喂给 CLI（字节层已分开）
- MAVLink 签名帧（多 13 字节）未处理：本工程不用签名
- `usb_cli_drop_count()` 暂未接入 `status` 展示

已知限制都集中在「两种数据共用一个口」这个前提上，用起来注意别在 CLI 里粘二进制。
