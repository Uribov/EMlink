// ============================================================
// ratelimit.cpp —— 按 QoS 的令牌桶流量整形（P1-10）
// 计价单位 = 空中毫秒（airtime），与 duty 同口径；控制流有保底份额。
// ============================================================
#include "ratelimit.h"
#include "../link/frame_codec.h"

static uint32_t g_ctrl_tokens = 0;
static uint32_t g_user_tokens = 0;
static uint32_t g_last_ms = 0;

static uint32_t g_ctrl_frac = 0;   // 控制桶未满 1 令牌的余量（0.01 令牌为单位）
static uint32_t g_user_frac = 0;

static uint32_t g_drop = 0;
static uint32_t g_ctrl_drop = 0;
static uint32_t g_oversize = 0;

void ratelimit_init(void) {
    g_ctrl_tokens = 0;
    g_user_tokens = 0;
    g_ctrl_frac = 0;
    g_user_frac = 0;
    g_last_ms = millis();
    g_drop = 0;
    g_ctrl_drop = 0;
    g_oversize = 0;
}

static void rl_refill(uint32_t now) {
    uint32_t dt = now - g_last_ms;
    if (dt == 0) return;
    g_last_ms = now;
    // 不要写成 dt*PCT/100：mac_send 高频调用（dt<10ms）时整数除法会把余量全丢，
    // 控制桶永远补不回来 —— 而那正是限流最该起作用的时刻。这里用余量累加。
    g_ctrl_frac   += dt * RL_CTRL_RESERVE_PCT;           // dt*10 → 百分之一令牌
    g_ctrl_tokens += g_ctrl_frac / 100;
    g_ctrl_frac   %= 100;

    g_user_frac   += dt * (100 - RL_CTRL_RESERVE_PCT);   // dt*90
    g_user_tokens += g_user_frac / 100;
    g_user_frac   %= 100;

    if (g_ctrl_tokens > RL_CTRL_BURST_MS) g_ctrl_tokens = RL_CTRL_BURST_MS;
    if (g_user_tokens > RL_USER_BURST_MS) g_user_tokens = RL_USER_BURST_MS;
}

bool ratelimit_allow(uint8_t qos, uint32_t airtime_ms) {
    if (airtime_ms == 0) return true;
    rl_refill(millis());

    // 单帧就超过桶容量（例：SF10 的 179B 分片 ≈1.64s > 100ms）：
    // 一律拒绝会把这些帧永久丢光（与 duty 的 P1-2 例外同构）→ 放行 + 计数
    if (qos == QOS2) {
        if (airtime_ms > RL_CTRL_BURST_MS) { g_oversize++; return true; }
        if (g_ctrl_tokens >= airtime_ms) { g_ctrl_tokens -= airtime_ms; return true; }
        g_ctrl_drop++; g_drop++;
        return false;
    }
    if (airtime_ms > RL_USER_BURST_MS) { g_oversize++; return true; }
    if (g_user_tokens >= airtime_ms) { g_user_tokens -= airtime_ms; return true; }
    g_drop++;
    return false;
}

uint32_t ratelimit_drop_count(void)        { return g_drop; }
uint32_t ratelimit_ctrl_drop_count(void)   { return g_ctrl_drop; }
uint32_t ratelimit_oversize_count(void)    { return g_oversize; }
