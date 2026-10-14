#pragma once
// ============================================================
// pinmap.h —— 引脚映射入口（唯一真相源）
//
// 多方案架构：本文件不硬编码任何引脚，只按编译宏 include 板级 + 射频级。
//   板级（飞控 UART / LED / 悬空脚）  -> boards/<板>.h
//   射频级（SPI / DIO / BUSY / NRST / RXEN / TXEN）-> radios/<射频>.h
// 编译时用 -DBOARD_XXX 选板、-DRADIO_XXX 选射频（见 platformio.ini 各 [env]）。
// 一个方案 = 一块板 + 一个射频模块。
// ============================================================

// ---- 板级 ----
#if defined(BOARD_ESP32S3_SUPERMINI)
  #include "boards/esp32-s3-supermini.h"
#else
  #error "缺板级宏：加 -DBOARD_XXX（如 -DBOARD_ESP32S3_SUPERMINI）"
#endif

// ---- 射频级 ----
#if defined(RADIO_E220_900M22S)
  #include "radios/e220-900m22s.h"
#else
  #error "缺射频宏：加 -DRADIO_XXX（如 -DRADIO_E220_900M22S）"
#endif

// ---- USB 检测（无需引脚）----
// USB 是否连上主机用 TinyUSB 枚举判断（见 power_detect），
// 供电来源不靠测电压区分（C 口/4Pin 供电时 5V 引脚都有电）。
