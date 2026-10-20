# uart_fc —— 飞控 UART

## 职责

与飞控的串口，DMA 接收 + 环形缓冲，保证 MAVLink 连续字节流不丢。
仅 AIR 角色使用，GCS/RELAY 不初始化本串口。

它只做字节搬运，不解析 MAVLink；解析在 mav_router 里。


## 依赖

- `pinmap.h`：`PIN_FC_TX` / `PIN_FC_RX` / `FC_UART_NUM`
- FreeRTOS 队列：`uart_task` 从缓冲取数投 `q_uart_rx`

依赖只有引脚定义和一个队列，不引用上层模块。


## 关键实现点

1. 用 UART0（`FC_UART_NUM=0`，TX=GPIO43 / RX=GPIO44）；USB 日志走 USB-OTG 的 `Serial`，不占 UART0，无冲突。飞控数据用 `Serial0` 收发，别用 `Serial`
2. 接收用 `uart_driver_install` + DMA 事件回调写环形缓冲（8KB 起）
3. 发送用 `uart_write_bytes`（自带 TX FIFO，非阻塞）
4. 波特率从 NVS `uart_baud` 读，默认 115200

第 1 条是排查串口问题时的第一个检查点：飞控数据必须走 `Serial0`，走错口就是黑洞。


## 对外接口

见 `uart_fc.h`。要点：`uart_fc_init / uart_fc_start / uart_fc_write` + `uart_fc_drop_count()`（上行队列满丢弃计数，P1-10）。

对外三个函数加一个计数 getter，收发方向各一个入口。


## 验收（P0）

- 飞控串口回环：自发自收无丢字节；波特率 115200 长时间跑无溢出

回环能验字节完整性，长跑能验缓冲深度够不够。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

飞控接在 UART0（GPIO43=TX / GPIO44=RX），这个模块就是「搬运工」—— 把飞控发来的字节搬进队列、把队列里的字节发给飞控。


### 用的串口

- `Serial0`（UART0），飞控数据专用
- `Serial`（USB 虚拟串口）是调试口 —— CLI 与 MAVLink 共用这一个口（单口字节分流，见 `usb_cdc.md`）；`Serial1` 不存在（从未 begin 的 UART1 = 黑洞，别用）

三个串口对象里只有 `Serial0` 是给飞控的，另外两个都属于 USB 口。


### 数据流（uart_task，核1 优先级10，1ms 轮询）

```
飞控 TX ──> Serial0.available()/read ──> q_uart_rx ──> mav_router（解析）
飞控 RX <── Serial0.write <── q_uart_tx <── mav_router（下发）
```

上下行各走一个队列，1ms 轮询一次。


### 实现取舍

- 当前用 Arduino `HardwareSerial` 的轮询搬运（1ms 一次），简单够用
- 架构 §3.1 说的「DMA 环形缓冲」（`uart_driver_install` + 事件回调）是升级项，P0 验证通过后、若发现高波特率丢字节再换

现在是轮询版，够用就不换；真要换的触发条件是「高波特率下出现丢字节」。
