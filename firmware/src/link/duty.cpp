// ============================================================
// duty.cpp —— 占空比限流：滑动窗口，限制每秒空中发射时间
// ============================================================
#include "duty.h"

static uint32_t g_max_ms = DUTY_MAX_MS_PER_SEC;
static uint32_t g_window_start = 0;
static uint32_t g_used_ms = 0;

bool duty_reserve(uint32_t airtime_ms) {
    uint32_t now = millis();
    if (now - g_window_start >= 1000) {
        g_window_start = now;
        g_used_ms = 0;
    }
    if (g_used_ms + airtime_ms > g_max_ms) return false;
    g_used_ms += airtime_ms;
    return true;
}

void duty_set_max(uint32_t ms_per_sec) {
    g_max_ms = ms_per_sec;
}
