#pragma once
// ============================================================
// power_detect —— USB 是否连上主机（TinyUSB 枚举检测）
// 依赖：TinyUSB（Arduino 内置）；规格：power_detect.md
// ============================================================
#include <Arduino.h>

// 初始化（启用 USB 枚举状态检测）
void power_detect_init();

// USB 是否已与主机握手（枚举成功）。供电来源不靠测电压区分。
bool power_usb_present();

// 事件发布：插入/拔出时 set event bits（EV_USB_PLUG / EV_USB_UNPLUG）
// 由本模块内部调用，供 failsafe_policy 订阅
