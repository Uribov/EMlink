// ============================================================
// power_detect.cpp —— USB 是否连上主机（TinyUSB 枚举检测）
// 供电来源不靠测电压区分（C 口/4Pin 供电时 5V 引脚都有电）。
// 事件发布在 power_usb_present() 内（去抖：状态翻转才发，且由周期调用自然去抖）。
// ============================================================
#include "power_detect.h"
#include "../rtos.h"
#include "esp32-hal-tinyusb.h"

static bool g_last_present = false;

void power_detect_init() {
    g_last_present = tud_mounted();
}

bool power_usb_present() {
    bool now = tud_mounted();
    if (now != g_last_present) {
        if (evg) {
            if (now) xEventGroupSetBits(evg, EV_USB_PLUG);
            else     xEventGroupSetBits(evg, EV_USB_UNPLUG);
        }
        g_last_present = now;
    }
    return now;
}
