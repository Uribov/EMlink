// ============================================================
// stats.cpp —— 周期统计、LED 状态、喂狗
// 依赖各模块统计接口；约定见架构 §10
// ============================================================
#include "stats.h"
#include <string.h>
#include "../rtos.h"
#include "../pinmap.h"
#include "../net/mesh.h"
#include "../link/mac.h"
#include "../service/nvs_cfg.h"
#include "../service/frag.h"
#include "../service/watchdog.h"
#include "../service/ota.h"
#include "../drivers/radio.h"
#include "../drivers/power_detect.h"
#include "../drivers/usb_cdc.h"   // Z1：stats 输出走 USB CLI 出口（Serial1 是未初始化的 UART1 = 黑洞）
#include "../app/role.h"
#include "../app/failsafe_policy.h"
#include "../net/route.h"
#include <Adafruit_NeoPixel.h>

#define LED_BRIGHT_NORMAL   20      // 常规亮度（原有）
#define WIFI_LED_BRIGHT     60      // 维护模式稍亮，彩虹看得清
#define WIFI_LED_CYCLE_MS   2500    // 彩虹一轮：2.5s（用户定；原 1.5s 太快）
#define WIFI_LED_STEPS      21      // 7 基色 + 每段 2 个插值（含紫→红，共 7 段）

#define BOOT_SEQ_MS         1500    // 上电轮闪总时长 1.5s（用户 2026-10-17 定；2s→1.5s）
#define BOOT_GAP_MS         500     // 轮闪结束后灭灯 0.5s（空档，分隔"启动"与"工作"两阶段）
#define BOOT_STEP_MS        83      // 红→蓝→绿 每色时长（不变）
#define BLINK_LOST_HALF_MS  100     // 快闪：5 次/秒 → 半周期 100ms
#define BLINK_IDLE_HALF_MS  500     // 慢闪：1 次/秒 → 半周期 500ms
#define LED_DEBOUNCE_MS     500     // 状态切换去抖（led_states.md 要求）

#define LED_TASK_STACK      2048
#define LED_TASK_PRIO       1
#define LED_TASK_CORE       1        // 与 loopTask 同核，避开 core0 的 WiFi/射频
#define LED_TASK_PERIOD_MS  20       // 50Hz：对 83ms/200ms 的相位精度足够

static Adafruit_NeoPixel* g_led = nullptr;
static TaskHandle_t g_stats_task = nullptr;

static uint32_t g_led_t0 = 0;       // 启动序列计时基准（首次调用时刻）
static uint8_t  g_led_key = 3;      // 当前健康状态：0=故障 1=丢链 2=待机 3=正常
static uint32_t g_led_key_ms = 0;   // 上次状态切换时刻（去抖用）

static uint16_t g_failsafe_s = 3;

// 本帧实际画出去的灯态（供 CLI/上位机查询；tick 内每帧更新）
static led_snapshot_t g_led_snap = {"off", 0, 0, 0, 0, 0};

// ---- 灯态变化主动推送（F8a：事件驱动，替代上位机轮询）----
#define LED_STREAM_MIN_GAP_MS 50    // 防抖：同一帧边界不重复推

static bool g_led_stream = true;    // 默认开
static const char* g_last_mode = "";
static uint8_t  g_last_r = 0, g_last_g = 0, g_last_b = 0;
static uint16_t g_last_half = 0;
static uint32_t g_last_push_ms = 0;

// 本帧 snapshot 写完后调用：仅在灯态变化时推一行 "#led ..."
static void led_stream_sync(uint32_t now) {
    bool changed = (g_last_mode != g_led_snap.mode &&
                    strcmp(g_last_mode, g_led_snap.mode) != 0) ||
                   g_last_r != g_led_snap.r ||
                   g_last_g != g_led_snap.g ||
                   g_last_b != g_led_snap.b ||
                   g_last_half != g_led_snap.half_ms;

    // 关闭时仍更新"上一帧"，重新打开后不会把旧差异一次补推出去
    if (!g_led_stream) {
        g_last_mode = g_led_snap.mode;
        g_last_r = g_led_snap.r; g_last_g = g_led_snap.g; g_last_b = g_led_snap.b;
        g_last_half = g_led_snap.half_ms;
        return;
    }
    if (!changed) return;
    if ((now - g_last_push_ms) < LED_STREAM_MIN_GAP_MS) return;

    char line[64];
    snprintf(line, sizeof(line), "#led mode=%s color=%u,%u,%u half=%u\r\n",
             g_led_snap.mode, (unsigned)g_led_snap.r, (unsigned)g_led_snap.g,
             (unsigned)g_led_snap.b, (unsigned)g_led_snap.half_ms);
    usb_cli_write((const uint8_t*)line, strlen(line));

    g_last_mode = g_led_snap.mode;
    g_last_r = g_led_snap.r; g_last_g = g_led_snap.g; g_last_b = g_led_snap.b;
    g_last_half = g_led_snap.half_ms;
    g_last_push_ms = now;
}

void stats_led_stream_set(bool on) {
    g_led_stream = on;
}

bool stats_led_stream_get(void) {
    return g_led_stream;
}

// 只初始化 LED（可在启动极早期调用，让启动序列尽早开跑；与 stats_task 无关）
void stats_led_init(void) {
#if PIN_LED >= 0
    if (g_led) return;
    g_led = new Adafruit_NeoPixel(1, PIN_LED, NEO_GRB + NEO_KHZ800);
    g_led->begin();
    g_led->setBrightness(LED_BRIGHT_NORMAL);
#endif
}

void stats_init() {
    stats_led_init();
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
    // 链路质量 = 发出去的帧里拿到 ACK 的比例（P1-10；原为恒 0 的占位）
    uint32_t ack_total = out->ack_ok + out->ack_miss;
    out->link_quality_pct = ack_total ? (uint8_t)(100u * out->ack_ok / ack_total) : 0;
}

void stats_print(const stats_t* s) {
    if (!s) return;
    // Z1：输出走 USB CLI 出口（usb_cli_write 内部有互斥锁，与 cli_print 同一个口）；
    // 绝不能用 Serial1 —— 那是从未 begin() 的 UART1 硬件串口，输出会进黑洞。
    char line[128];
    snprintf(line, sizeof(line),
             "fwd=%u ttl=%u dup=%u reasm_to=%u\r\n"
             "rssi=%d snr=%d heap=%u wdt=%u rf_reinit=%u\r\n",
             s->fwd, s->dropped_ttl, s->dropped_dup, s->reasm_timeout,
             s->rssi, s->snr, s->heap_free, s->wdt_resets, s->rf_reinit);
    usb_cli_write((const uint8_t*)line, strlen(line));
}

// 彩虹离散色阶：7 基色 + 相邻两色之间 2 个线性插值 → 21 档（最后一档连回红色，循环无缝）
static void rainbow_rgb(uint8_t idx, uint8_t* r, uint8_t* g, uint8_t* b) {
    static const uint8_t BASE[7][3] = {
        {255,   0,   0},   // 红
        {255, 128,   0},   // 橙
        {255, 255,   0},   // 黄
        {  0, 255,   0},   // 绿
        {  0, 255, 255},   // 青
        {  0,   0, 255},   // 蓝
        {128,   0, 255},   // 紫
    };
    uint8_t seg = (uint8_t)(idx / 3);          // 0..6：第几段
    uint8_t t   = (uint8_t)(idx % 3);          // 0=基色，1=1/3 插值，2=2/3 插值
    const uint8_t* a = BASE[seg];
    const uint8_t* c = BASE[(seg + 1) % 7];    // 末段回到红 → 无缝
    if (t == 0) {
        *r = a[0]; *g = a[1]; *b = a[2];
        return;
    }
    uint8_t n = (t == 1) ? 1 : 2;              // 分子：1/3 或 2/3
    *r = (uint8_t)((a[0] * (3 - n) + c[0] * n) / 3);
    *g = (uint8_t)((a[1] * (3 - n) + c[1] * n) / 3);
    *b = (uint8_t)((a[2] * (3 - n) + c[2] * n) / 3);
}

// LED 唯一刷灯入口：由独立 led_task 每 20ms 调用（S14；不再依赖 main loop）。
// 优先级：上电启动序列 > WiFi 维护模式彩虹 > 故障 > 丢链 > 待机 > 正常（见 led_states.md）
// 所有节奏都由 millis() 算相位，非阻塞；绝不在 LED 上 delay()。
void stats_led_tick(void) {
    if (!g_led) return;
    uint32_t now = millis();
    if (g_led_t0 == 0) g_led_t0 = now;          // 启动序列计时基准（首次调用时刻）
    uint32_t age = now - g_led_t0;

    // ---- 1) 上电启动：红→蓝→绿 循环快闪 1 秒 ----
    if (age < BOOT_SEQ_MS) {
        static const uint8_t SEQ[3][3] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
        uint8_t k = (uint8_t)((age / BOOT_STEP_MS) % 3);
        g_led_snap.mode = "boot";
        g_led_snap.r = SEQ[k][0]; g_led_snap.g = SEQ[k][1]; g_led_snap.b = SEQ[k][2];
        g_led_snap.half_ms = 0;
        g_led_snap.phase_ms = now;
        led_stream_sync(now);
        g_led->setBrightness(LED_BRIGHT_NORMAL);
        g_led->setPixelColor(0, g_led->Color(SEQ[k][0], SEQ[k][1], SEQ[k][2]));
        g_led->show();
        return;
    }
    if (age < BOOT_SEQ_MS + BOOT_GAP_MS) {          // 空档：灭灯 0.5s
        g_led_snap.mode = "boot";
        g_led_snap.r = 0; g_led_snap.g = 0; g_led_snap.b = 0;
        g_led_snap.half_ms = 0;
        g_led_snap.phase_ms = now;
        led_stream_sync(now);
        g_led->setBrightness(LED_BRIGHT_NORMAL);
        g_led->setPixelColor(0, g_led->Color(0, 0, 0));
        g_led->show();
        return;
    }

    // ---- 2) WiFi 维护模式：彩虹（2.5s 一轮）----
    // 色号 = 21 档色表（7 基色 + 每段 2 个插值色，见 rainbow_rgb），
    // 但档与档之间按小数部分连续插值 —— 避免"每 119ms 硬切一格"造成的顿挫感（用户 2026-10-17 反馈）。
    // 相位由 millis() 直接算，与 loop 实际抖动无关，所以不会累积漂移。
    if (ota_active()) {
        uint32_t phase = (now % WIFI_LED_CYCLE_MS) * WIFI_LED_STEPS;                  // 0 .. CYCLE*21
        uint8_t  i0    = (uint8_t)(phase / WIFI_LED_CYCLE_MS);                        // 0..20 当前色号
        uint8_t  t     = (uint8_t)(((phase % WIFI_LED_CYCLE_MS) * 256UL) / WIFI_LED_CYCLE_MS); // 0..255 档内位置

        uint8_t r0, g0, b0, r1, g1, b1;
        rainbow_rgb(i0, &r0, &g0, &b0);
        rainbow_rgb((uint8_t)((i0 + 1) % WIFI_LED_STEPS), &r1, &g1, &b1);

        uint8_t r = (uint8_t)(((uint16_t)r0 * (255 - t) + (uint16_t)r1 * t) / 255);
        uint8_t g = (uint8_t)(((uint16_t)g0 * (255 - t) + (uint16_t)g1 * t) / 255);
        uint8_t b = (uint8_t)(((uint16_t)b0 * (255 - t) + (uint16_t)b1 * t) / 255);

        g_led_snap.mode = "rainbow";
        g_led_snap.r = r; g_led_snap.g = g; g_led_snap.b = b;
        g_led_snap.half_ms = 0;
        g_led_snap.phase_ms = now;
        led_stream_sync(now);
        g_led->setBrightness(WIFI_LED_BRIGHT);
        g_led->setPixelColor(0, g_led->Color(r, g, b));
        g_led->show();
        return;
    }

    // ---- 3) 常态：角色色 × 健康节奏 ----
    uint8_t role = role_current();
    led_role_t lr = (role == ROLE_AIR)   ? LED_ROLE_AIR :
                    (role == ROLE_GCS)   ? LED_ROLE_GCS :
                    (role == ROLE_RELAY) ? LED_ROLE_RELAY : LED_ROLE_UNSET;

    uint8_t rr = 0, gg = 0, bb = 0;
    switch (lr) {
        case LED_ROLE_GCS:   gg = 255; break;
        case LED_ROLE_AIR:   bb = 255; break;
        case LED_ROLE_RELAY: rr = 128; bb = 255; break;   // 紫（青与蓝太接近，用户 2026-10-17 定）
        case LED_ROLE_UNSET: rr = 255; gg = 255; bb = 255; break;   // 白（S12）
    }

    // 状态码：0=故障 1=丢链 2=待机 3=正常（优先级从高到低）
    neighbor_t nb[1];
    uint8_t key;
    if (!radio_is_ready())                 key = 0;
    else if (failsafe_link_lost())         key = 1;
    else if (route_neighbors(nb, 1) == 0)  key = 2;
    else                                   key = 3;

    // 去抖：切换未满 500ms 就维持旧状态
    if (key != g_led_key && (now - g_led_key_ms) < LED_DEBOUNCE_MS) key = g_led_key;
    else if (key != g_led_key) { g_led_key = key; g_led_key_ms = now; }

    bool on = true;
    if (key == 0) { rr = 255; gg = 0; bb = 0;                        // 故障：红快闪
                    on = ((now / BLINK_LOST_HALF_MS) % 2) == 0; }
    else if (key == 1) on = ((now / BLINK_LOST_HALF_MS) % 2) == 0;   // 丢链：角色色快闪
    else if (key == 2) on = ((now / BLINK_IDLE_HALF_MS) % 2) == 0;   // 待机：角色色慢闪
    if (!on) { rr = gg = bb = 0; }

    g_led_snap.mode     = (key == 0) ? "fault" :
                          (key == 1) ? "lost"  :
                          (key == 2) ? "idle"  : "ok";
    g_led_snap.r        = rr;
    g_led_snap.g        = gg;
    g_led_snap.b        = bb;
    g_led_snap.half_ms  = (key == 0) ? BLINK_LOST_HALF_MS :
                          (key == 1) ? BLINK_LOST_HALF_MS :
                          (key == 2) ? BLINK_IDLE_HALF_MS : 0;
    g_led_snap.phase_ms = now;
    led_stream_sync(now);

    g_led->setBrightness(LED_BRIGHT_NORMAL);
    g_led->setPixelColor(0, g_led->Color(rr, gg, bb));
    g_led->show();
}

void stats_led_snapshot(led_snapshot_t* out) {
    if (!out) return;
    if (!g_led) {
        out->mode = "off";
        out->r = out->g = out->b = 0;
        out->half_ms = 0;
        out->phase_ms = millis();
        return;
    }
    *out = g_led_snap;
}

static void led_task_func(void* arg) {
    (void)arg;
    for (;;) {
        stats_led_tick();
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
    }
}

// 启动 LED 刷新任务：此后灯由本任务独立驱动，不再依赖 main loop
// （射频探测阻塞 ~8s 期间 loop 走不动，灯照样要闪）
void stats_led_start(void) {
    xTaskCreatePinnedToCore(led_task_func, "led", LED_TASK_STACK,
                            nullptr, LED_TASK_PRIO, nullptr, LED_TASK_CORE);
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

        // LED 不再由 1s 周期的 stats_task 驱动：统一由独立 led_task 每 20ms 刷新

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void stats_start(void) {
    xTaskCreatePinnedToCore(stats_task_func, "stats", STATS_TASK_STACK,
                            nullptr, STATS_TASK_PRIO, &g_stats_task, STATS_TASK_CORE);
}
