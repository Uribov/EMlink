#pragma once
// ============================================================
// failsafe_policy —— 故障安全策略 + 电源/接口状态机
// 依赖：power_detect.h、mav_router.h；约定见架构 §8.2
// ============================================================
#include <Arduino.h>

// 电源/接口状态
enum : uint8_t {
    PWR_CFG_ONLY = 0,   // 态A：仅 USB 配置（role 未设）
    PWR_AIR_UART = 1,   // 态B：AIR 仅串口数据通路
    PWR_GCS_USB  = 2,   // 态C：GCS USB 跑 MAVLink
};

// 初始化
void failsafe_init();

// 上电进入状态机（role_apply 之后调用）
void failsafe_enter();

// 事件处理（EV_USB_PLUG / EV_USB_UNPLUG / EV_LINK_LOST / EV_LINK_RESTORED）
void failsafe_on_event(uint32_t ev);

// 当前电源态
uint8_t failsafe_power_state();

// 链路是否丢失
bool failsafe_link_lost();
