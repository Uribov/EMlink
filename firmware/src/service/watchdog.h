#pragma once
// ============================================================
// watchdog —— 双看门狗 + 射频卡死自救
// 依赖：esp_task_wdt、radio.h；约定见架构 §8.2
// ============================================================
#include <Arduino.h>

// 初始化（任务级 SW WDT + 硬件 WDT）：只做初始化，不订阅任何任务
void watchdog_init();

// 订阅当前任务到 WDT：调用后必须在该任务循环里周期调用 watchdog_feed()
void watchdog_register_current();

// 喂狗：喂的是当前任务，必须由已订阅的任务自己调用
void watchdog_feed();

// 射频故障上报（射频芯片连续无响应时）：
// 先 radio_hardReset() 软复位射频，连续 N 次仍坏 → 整机复位
void watchdog_rf_fault();

// 统计
uint32_t watchdog_wdt_resets();
uint32_t watchdog_rf_reinits();
