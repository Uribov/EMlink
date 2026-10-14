#pragma once
// ============================================================
// watchdog —— 双看门狗 + 射频卡死自救
// 依赖：esp_task_wdt、radio.h；约定见架构 §8.2
// ============================================================
#include <Arduino.h>

// 初始化（任务级 SW WDT + 硬件 WDT）
void watchdog_init();

// 喂狗（各长任务周期调用）
void watchdog_feed();

// 射频故障上报（射频芯片连续无响应时）：
// 先 radio_hardReset() 软复位射频，连续 N 次仍坏 → 整机复位
void watchdog_rf_fault();

// 统计
uint32_t watchdog_wdt_resets();
uint32_t watchdog_rf_reinits();
