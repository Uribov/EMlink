// ============================================================
// stats.cpp —— 周期统计、LED 状态、喂狗
// 依赖各模块统计接口；约定见架构 §10
// ============================================================
#include "stats.h"
#include "../rtos.h"
#include "../pinmap.h"
#include "../net/mesh.h"
#include "../link/mac.h"
#include "../service/nvs_cfg.h"
#include "../service/frag.h"
#include "../service/watchdog.h"
#include "../drivers/radio.h"
#include "../drivers/power_detect.h"
#include "../app/role.h"
#include "../app/failsafe_policy.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel* g_led = nullptr;
static TaskHandle_t g_stats_task = nullptr;

static uint16_t g_failsafe_s = 3;

void stats_init() {
#if PIN_LED >= 0
    g_led = new Adafruit_NeoPixel(1, PIN_LED, NEO_GRB + NEO_KHZ800);
    g_led->begin();
    g_led->setBrightness(20);
#endif
    cfg_t c;
    if (nvs_cfg_load(&c) && c.failsafe_s != 0) g_failsafe_s = c.failsafe_s;
}

void stats_collect(stats_t* out) {
    if (!out) return;
    out->fwd          = mesh_fwd_count();
    out->dropped_ttl  = mesh_drop_ttl_count();
    out->dropped_dup  = mesh_drop_dup_count();
    out->dropped_crc  = mesh_drop_crc_count();
    out->reasm_timeout = frag_timeout_count();
    out->tx_frames    = mac_tx_count();
    out->rx_frames    = mesh_rx_count();
    out->ack_ok       = mac_ack_ok();
    out->ack_miss     = mac_ack_miss();
    out->retries      = mac_retries();
    out->rssi         = (int16_t)radio_lastRssi();
    out->snr          = (int16_t)radio_lastSnr();
    out->heap_free    = ESP.getFreeHeap();
    out->wdt_resets   = watchdog_wdt_resets();
    out->rf_reinit    = watchdog_rf_reinits();
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

static bool g_link_was_lost = false;

static void stats_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    for (;;) {
        watchdog_feed();
        power_usb_present();   // 内部做 USB 插拔事件检测

        // BEACON 由 adr 模块统一发（mac_task 里 5s 周期），此处不再发

        // 失联判定（架构 §8.2：failsafe_s 秒无上行 → EV_LINK_LOST）
        bool link_lost = (millis() - mesh_last_rx_ms()) > g_failsafe_s * 1000;
        if (link_lost && !g_link_was_lost) {
            xEventGroupSetBits(evg, EV_LINK_LOST);
            g_link_was_lost = true;
        } else if (!link_lost && g_link_was_lost) {
            xEventGroupSetBits(evg, EV_LINK_RESTORED);
            g_link_was_lost = false;
        }

        // 事件处理：交给 failsafe_policy（电源/链路状态机）
        EventBits_t ev = xEventGroupGetBits(evg) &
                         (EV_USB_PLUG | EV_USB_UNPLUG | EV_LINK_LOST | EV_LINK_RESTORED);
        if (ev) {
            failsafe_on_event(ev);
            xEventGroupClearBits(evg, ev);
        }

        // LED：角色 + 健康
        uint8_t role = role_current();
        led_role_t lr = (role == ROLE_AIR)   ? LED_ROLE_AIR :
                        (role == ROLE_GCS)   ? LED_ROLE_GCS :
                        (role == ROLE_RELAY) ? LED_ROLE_RELAY : LED_ROLE_UNSET;
        led_health_t lh = failsafe_link_lost() ? LED_HEALTH_LOST : LED_HEALTH_OK;
        if (!radio_is_ready()) lh = LED_HEALTH_FAULT;   // 射频未就绪：整灯红（覆盖角色色）
        stats_update_led(lr, lh);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void stats_start(void) {
    xTaskCreatePinnedToCore(stats_task_func, "stats", STATS_TASK_STACK,
                            nullptr, STATS_TASK_PRIO, &g_stats_task, STATS_TASK_CORE);
}
