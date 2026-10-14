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
见 `uart_fc.h`。

## 验收（P0）
- 飞控串口回环：自发自收无丢字节；波特率 115200 长时间跑无溢出
