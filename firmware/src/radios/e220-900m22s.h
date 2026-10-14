#pragma once
// ============================================================
// 射频模块：E220-900M22S（LLCC68 / SX1268 类，SPI，22dBm）
// 方案标识：RADIO_E220_900M22S
// 驱动：RadioLib SX1268（drivers/radio）
// ============================================================

// 芯片类型（驱动层据此选实现；将来加 SX127x 等在此扩展）
#define RADIO_SX126X    1     // SX1261/2/8 + LLCC68 同族

// ---- SPI ----
#define PIN_LORA_NSS    8     // 片选，E220 脚 19
#define PIN_LORA_SCK    9     // 时钟，E220 脚 18
#define PIN_LORA_MOSI   10    // 数据，E220 脚 17
#define PIN_LORA_MISO   11    // 数据，E220 脚 16

// ---- 控制 ----
#define PIN_LORA_DIO1   7     // 中断，E220 脚 13
#define PIN_LORA_BUSY   13    // 忙检测，E220 脚 14
#define PIN_LORA_NRST   12    // 复位 active-low，E220 脚 15
#define PIN_LORA_RXEN   1     // 收使能 active-high，E220 脚 6
#define PIN_LORA_TXEN   2     // 发使能 active-high，E220 脚 7
