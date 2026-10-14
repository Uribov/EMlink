#pragma once
// ============================================================
// usb_cdc —— 单口分流：MAVLink 数据 + CLI 共用 Serial 一个虚拟串口
//   usb_task 按字节分流：0xFD/0xFE 帧头收 MAVLink 帧，其余作 CLI 文本
// 依赖：ESP32-S3 USB-OTG（Arduino 内置）；规格：usb_cdc.md
// ============================================================
#include <Arduino.h>

void usb_cdc_init();

// ---- MAVLink 数据通路（接 QGC，走共用 Serial 口）----
// 注：接收已由 usb_task 统一接管；usb_mav_read/available 仅为兼容保留。
bool   usb_mav_available();
size_t usb_mav_read(uint8_t* buf, size_t maxlen);
size_t usb_mav_write(const uint8_t* buf, size_t len);

// ---- CLI 命令行（配置 / 调试，走共用 Serial 口）----
bool   usb_cli_available();
size_t usb_cli_read(uint8_t* buf, size_t maxlen);
size_t usb_cli_write(const uint8_t* buf, size_t len);

// 启动 USB CDC 任务（内部 xTaskCreate usb_task_func，任务函数为模块内 static）
void usb_cdc_start(void);

// 地面站下行队列满丢弃数（P1-10）
uint32_t usb_cdc_drop_count(void);

// CLI 文本环形缓冲满丢弃数（P1-6 单口分流）
uint32_t usb_cli_drop_count(void);
