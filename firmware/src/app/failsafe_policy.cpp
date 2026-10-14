// ============================================================
// failsafe_policy.cpp —— 故障安全 + 电源/接口状态机
// 固件绝不越权替飞控做决定（架构 §8.2）。
// ============================================================
#include "failsafe_policy.h"
#include "../rtos.h"
#include "../drivers/power_detect.h"
#include "../service/nvs_cfg.h"

static uint8_t g_power_state = PWR_CFG_ONLY;
static bool    g_link_lost = false;

void failsafe_init() {
    g_power_state = PWR_CFG_ONLY;
    g_link_lost = false;
}

void failsafe_enter() {
    cfg_t c;
    nvs_cfg_load(&c);
    if (c.role == ROLE_UNSET)      g_power_state = PWR_CFG_ONLY;
    else if (c.role == ROLE_AIR)   g_power_state = PWR_AIR_UART;
    else if (c.role == ROLE_GCS)   g_power_state = PWR_GCS_USB;
    else                           g_power_state = PWR_CFG_ONLY;
}

void failsafe_on_event(uint32_t ev) {
    if (ev & EV_LINK_LOST)     g_link_lost = true;
    if (ev & EV_LINK_RESTORED) g_link_lost = false;

    // 飞行中（AIR 态）USB 枚举变化：只弹 CLI，绝不切换数据通路（防数据流被 USB 抢走）
    if (g_power_state == PWR_AIR_UART && (ev & (EV_USB_PLUG | EV_USB_UNPLUG))) {
        // 数据通路保持不变，仅 CLI 可用（由 usb_cdc 的 CDC1 提供）
    }
}

uint8_t failsafe_power_state() { return g_power_state; }
bool    failsafe_link_lost()   { return g_link_lost; }
