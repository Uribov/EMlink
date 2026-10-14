// ============================================================
// stats.cpp —— 周期统计、LED 状态、喂狗
// 依赖各模块统计接口；约定见架构 §10
// ============================================================
#include "stats.h"
#include "../rtos.h"
#include "../pinmap.h"
#include "../net/mesh.h"
#include "../service/frag.h"
#include "../service/watchdog.h"
#include "../drivers/radio.h"
#include "../drivers/power_detect.h"
#include "../app/role.h"
#include "../app/failsafe_policy.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel* g_led = nullptr;
static TaskHandle_t g_stats_task = nullptr;

void stats_init() {
#if PIN_LED >= 0
    g_led = new Adafruit_NeoPixel(1, PIN_LED, NEO_GRB + NEO_KHZ800);
    g_led->begin();
    g_led->setBrightness(20);
#endif
}

void stats_collect(stats_t* out) {
    if (!out) return;
    out->fwd          = mesh_fwd_count();
    out->dropped_ttl  = mesh_drop_ttl_count();
    out->dropped_dup  = mesh_drop_dup_count();
    out->reasm_timeout = frag_timeout_count();
    out->rssi         = (int16_t)radio_lastRssi();
    out->snr          = (int16_t)radio_lastSnr();
    out->heap_free    = ESP.getFreeHeap();
    out->wdt_resets   = watchdog_wdt_resets();
    out->rf_reinit    = watchdog_rf_reinits();
    // tx/rx/ack 统计由 mac/mesh 计数（接口待补，先占位 0）
    out->tx_frames = out->rx_frames = 0;
    out->ack_ok = out->ack_miss = out->retries = 0;
    out->dropped_crc = 0;
    out->link_quality_pct = 0;
}

void stats_print(const stats_t* s) {
    if (!s) return;
    // 打印到 CLI（用 Serial1）—— 由 cli 调用
    char line[128];
    snprintf(line, sizeof(line),
             "fwd=%u ttl=%u dup=%u reasm_to=%u\r\n"
             "rssi=%d snr=%d heap=%u wdt=%u rf_reinit=%u\r\n",
             s->fwd, s->dropped_ttl, s->dropped_dup, s->reasm_timeout,
             s->rssi, s->snr, s->heap_free, s->wdt_resets, s->rf_reinit);
    Serial1.print(line);
}

// LED：颜色=角色，节奏=健康；fault/reset 整灯红
void stats_update_led(led_role_t role, led_health_t health) {
    if (!g_led) return;
    uint8_t r = 0, g = 0, b = 0;
    switch (role) {
        case LED_ROLE_GCS:   g = 255; break;
        case LED_ROLE_AIR:   b = 255; break;
        case LED_ROLE_RELAY: g = 255; b = 255; break;
        case LED_ROLE_UNSET: r = 255; g = 255; break;
    }
    if (health == LED_HEALTH_FAULT || health == LED_HEALTH_RESET) {
        r = 255; g = 0; b = 0;   // 红覆盖角色色
    }
    g_led->setPixelColor(0, g_led->Color(r, g, b));
    g_led->show();
}

static void stats_task_func(void* arg) {
    (void)arg;
    for (;;) {
        watchdog_feed();
        power_usb_present();   // 内部做 USB 插拔事件检测

        // LED：角色 + 健康
        uint8_t role = role_current();
        led_role_t lr = (role == ROLE_AIR)   ? LED_ROLE_AIR :
                        (role == ROLE_GCS)   ? LED_ROLE_GCS :
                        (role == ROLE_RELAY) ? LED_ROLE_RELAY : LED_ROLE_UNSET;
        led_health_t lh = failsafe_link_lost() ? LED_HEALTH_LOST : LED_HEALTH_OK;
        stats_update_led(lr, lh);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void stats_start(void) {
    xTaskCreatePinnedToCore(stats_task_func, "stats", STATS_TASK_STACK,
                            nullptr, STATS_TASK_PRIO, &g_stats_task, STATS_TASK_CORE);
}
