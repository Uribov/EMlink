#pragma once
// ============================================================
// ratelimit —— 按 QoS 的令牌桶流量整形（P1-10）
// 计价单位 = 空中毫秒（airtime），与 duty 同口径；控制流有保底份额。
// ============================================================
#include <Arduino.h>

#define RL_CTRL_RESERVE_PCT   10   // 控制流(QOS2)保底份额（% 的每秒空口时间）
#define RL_CTRL_BURST_MS      30   // 控制桶容量（突发上限，ms）
#define RL_USER_BURST_MS     100   // 用户桶容量（ms）
#define RL_WINDOW_MS        1000   // 份额基准周期（ms，仅用于注释/换算）

void     ratelimit_init(void);

// 准入判定：true=放行（已扣令牌）；false=超限（调用方丢弃并计数，**不要重试/死等**）
bool     ratelimit_allow(uint8_t qos, uint32_t airtime_ms);

// 诊断
uint32_t ratelimit_drop_count(void);        // 因限流被丢的帧数（全部类）
uint32_t ratelimit_ctrl_drop_count(void);   // 其中控制类（>0 说明连保底份额都不够了）
uint32_t ratelimit_oversize_count(void);    // 单帧就超过桶容量而被放行的次数
