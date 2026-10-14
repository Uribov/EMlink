#pragma once
// ============================================================
// duty —— 占空比限流：限制每节点每秒空中发射时间
// 依赖：无；约定见架构 §5「占空比限制」
// ============================================================
#include <Arduino.h>

// 每周期(1s)允许的最大空中时间 ms（10% 占空比，可被 NVS 覆盖）
#define DUTY_MAX_MS_PER_SEC   100

// 申请发射时长 airtime_ms；true=放行，false=超限需等待（mac 层排队）
bool duty_reserve(uint32_t airtime_ms);

// 单帧超预算 → 放行并计数（例：SF10 的 179B 分片 ≈1.64s）；返回累计放行次数
uint32_t duty_oversize_count(void);
