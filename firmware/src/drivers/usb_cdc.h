#pragma once
// ============================================================
// usb_cdc —— 双虚拟串口：CDC0=MAVLink 数据，CDC1=CLI 命令行
// 依赖：ESP32-S3 USB-OTG + TinyUSB（Arduino 内置）；规格：usb_cdc.md
// ============================================================
#include <Arduino.h>

void usb_cdc_init();

// ---- CDC0：MAVLink 数据通路（接 QGC）----
bool   usb_mav_available();
size_t usb_mav_read(uint8_t* buf, size_t maxlen);
size_t usb_mav_write(const uint8_t* buf, size_t len);

// ---- CDC1：CLI 命令行（配置 / 调试）----
bool   usb_cli_available();
size_t usb_cli_read(uint8_t* buf, size_t maxlen);
size_t usb_cli_write(const uint8_t* buf, size_t len);

// 启动 USB CDC 任务（内部 xTaskCreate usb_task_func，任务函数为模块内 static）
void usb_cdc_start(void);
