// ============================================================
// watchdog.cpp —— 双看门狗 + 射频卡死自救
// 依赖 esp_task_wdt / radio.h；约定见架构 §8.2
// ============================================================
#include "watchdog.h"
#include "../rtos.h"
#include "../drivers/radio.h"
#include <esp_task_wdt.h>

static uint32_t g_wdt_resets = 0;
static uint32_t g_rf_reinits = 0;
static uint8_t  g_rf_fault_count = 0;

void watchdog_init() {
    esp_task_wdt_init(5, true);   // 5s 超时，超时 panic
    esp_task_wdt_add(nullptr);    // 把调用者（loopTask）加入 WDT
}

void watchdog_feed() {
    esp_task_wdt_reset();
}

void watchdog_rf_fault() {
    g_rf_fault_count++;
    if (g_rf_fault_count >= 3) {
        // 连续 3 次射频故障：整机复位兜底
        g_wdt_resets++;
        esp_restart();
    } else {
        // 先软复位射频芯片（NRST）
        radio_hardReset();
        g_rf_reinits++;
    }
}

uint32_t watchdog_wdt_resets()  { return g_wdt_resets; }
uint32_t watchdog_rf_reinits()  { return g_rf_reinits; }
