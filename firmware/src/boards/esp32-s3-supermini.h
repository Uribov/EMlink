#pragma once
// ============================================================
// 板级引脚：ESP32-S3 SuperMini（ESP32-S3FH4R2：4MB Flash + 2MB PSRAM）
// 方案标识：BOARD_ESP32S3_SUPERMINI
// 本文件只放「跟这块 MCU 板有关」的引脚；射频引脚见 ../radios/
// ============================================================

// ---- 飞控 UART（UART0）----
#define PIN_FC_TX       43    // U0TXD，S3 -> 飞控 RX
#define PIN_FC_RX       44    // U0RXD，S3 <- 飞控 TX
#define FC_UART_NUM     0     // 飞控数据用 Serial0；Serial 是 USB 日志

// ---- 状态 LED（WS2818 RGB，DIN）----
#define PIN_LED         48

// ---- 引脚占用 / 悬空说明 ----
// 已用：1,2,7,8,9,10,11,12,13（射频）、43,44（飞控）、48（LED）
// GPIO0  = BOOT 按钮（strapping，按住上电进下载模式），勿占用
// GPIO3  = strapping 脚（上电决定启动模式），禁止接外设，保持悬空
// GPIO4/5/6 = 悬空，空闲可扩展外设
// GPIO45/46 = strapping 脚，勿用
