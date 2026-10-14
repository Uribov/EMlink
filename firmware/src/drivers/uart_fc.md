# uart_fc —— 飞控 UART

## 职责
与 Pixhawk 的串口，DMA 接收 + 环形缓冲，保证 MAVLink 连续字节流不丢。
**仅 AIR 角色使用**；GCS/RELAY 不初始化本串口。

## 依赖
- `pinmap.h`：`PIN_FC_TX` / `PIN_FC_RX` / `FC_UART_NUM`
- FreeRTOS 队列：`uart_task` 从缓冲取数投 `q_uart_rx`

## 关键实现点
1. 用 UART0（`FC_UART_NUM=0`，TX=GPIO43 / RX=GPIO44）；USB 日志走 USB-OTG 的 `Serial`，不占 UART0，无冲突。飞控数据用 `Serial0` 收发，别用 `Serial`
2. 接收用 `uart_driver_install` + DMA 事件回调写环形缓冲（8KB 起）
3. 发送用 `uart_write_bytes`（自带 TX FIFO，非阻塞）
4. 波特率从 NVS `uart_baud` 读，默认 115200

## 对外接口
见 `uart_fc.h`。要点：`uart_fc_init / uart_fc_start / uart_fc_write` + `uart_fc_drop_count()`（上行队列满丢弃计数，P1-10）。

## 验收（P0）
- 飞控串口回环：自发自收无丢字节；波特率 115200 长时间跑无溢出

## 当前实现架构（供人和子 agent 阅读）

### 一句话
飞控（Pixhawk）接在 **UART0**（GPIO43=TX / GPIO44=RX），这个模块就是"搬运工"——把飞控发来的字节搬进队列、把队列里的字节发给飞控。

### 用的串口
- `Serial0`（UART0），飞控数据专用
- `Serial` 是 USB 日志口，`Serial1` 是 CLI 口，都**不碰**飞控这条线，互不干扰

### 数据流（uart_task，核1 优先级10，1ms 轮询）
```
飞控 TX ──> Serial0.available()/read ──> q_uart_rx ──> mav_router（解析）
飞控 RX <── Serial0.write <── q_uart_tx <── mav_router（下发）
```

### 实现取舍
- 当前用 Arduino `HardwareSerial` 的轮询搬运（1ms 一次），简单够用
- 架构 §3.1 说的"DMA 环形缓冲"（`uart_driver_install` + 事件回调）是升级项，P0 验证通过后、若发现高波特率丢字节再换
