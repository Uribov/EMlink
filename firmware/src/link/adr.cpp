// ============================================================
// adr.cpp —— BEACON 锚定固定速率表
// 依赖 radio.h / mesh.h（发 BEACON）/ role.h（判断 GCS）
// 5s 周期：0~1s SF10（慢速窗），1~5s SF7（快速窗）。
// GCS 在周期起点发 BEACON（SF10）；非 GCS 停在 SF10 等 BEACON，收到后对齐。
// ============================================================
#include "adr.h"
#include "../drivers/radio.h"
#include "../net/mesh.h"
#include "../app/role.h"

static uint8_t  g_sf = ADR_SF_SLOW;   // 当前 SF（默认先停慢速等 BEACON）
static bool     g_has_beacon = false; // 是否已对齐到周期起点
static uint32_t g_epoch_ms  = 0;      // 本周期起点（对齐后有效）
static uint32_t g_last_beacon_ms = 0; // GCS 上次发 BEACON 时刻

void adr_init() {
    g_sf = ADR_SF_SLOW;
    g_has_beacon = false;
    g_epoch_ms = 0;
    g_last_beacon_ms = 0;
    radio_set_sf(ADR_SF_SLOW);   // 开机先停 SF10，保证能收到第一个 BEACON
}

// 收到 BEACON：对齐周期起点
void adr_on_beacon() {
    g_has_beacon = true;
    g_epoch_ms = millis();
    // BEACON 用 SF10 发，能收到说明本机当前也是 SF10，无需再切
}

// 计算当前时刻在 5s 周期内的位置（0~4999 ms）
static uint32_t phase_in_period(uint32_t now) {
    uint32_t p = (now - g_epoch_ms) % ADR_PERIOD_MS;
    return p;
}

// 根据周期相位，返回此刻应该用的 SF
static uint8_t sf_for_phase(uint32_t phase) {
    // t=0~1s 慢速窗（含 BEACON 发收）
    if (phase < ADR_SLOW_MS) return ADR_SF_SLOW;
    // 周期末尾 guard 窗：提前回 SF10 等下一个 BEACON（抵消空中传播延迟 ~200ms）
    if (phase >= ADR_PERIOD_MS - ADR_GUARD_MS) return ADR_SF_SLOW;
    // 其余时间快速窗 SF7
    return ADR_SF_FAST;
}

void adr_tick() {
    uint32_t now = millis();
    uint8_t role = role_current();

    if (role == ROLE_GCS) {
        // GCS：自己就是锚点，5s 一周期，起点发 BEACON（SF10）
        if (now - g_last_beacon_ms >= ADR_PERIOD_MS) {
            g_last_beacon_ms = now;
            g_epoch_ms = now;
            g_has_beacon = true;

            // 切到 SF10 再发 BEACON（保证收方在 SF10 能听到）
            if (g_sf != ADR_SF_SLOW) {
                radio_set_sf(ADR_SF_SLOW);
                g_sf = ADR_SF_SLOW;
            }
            mesh_send_beacon();
            return;   // 本 tick 就停 SF10，下个 tick 再按相位切
        }

        // 已对齐，按相位切 SF
        if (g_has_beacon) {
            uint32_t phase = phase_in_period(now);
            uint8_t want = sf_for_phase(phase);
            if (want != g_sf) {
                radio_set_sf(want);
                g_sf = want;
            }
        }
    } else {
        // 非 GCS（AIR/RELAY）：停在 SF10 等 BEACON；收到后按相位切
        if (!g_has_beacon) {
            // 还没收到过 BEACON，保持 SF10 傻等
            if (g_sf != ADR_SF_SLOW) {
                radio_set_sf(ADR_SF_SLOW);
                g_sf = ADR_SF_SLOW;
            }
            return;
        }

        uint32_t phase = phase_in_period(now);
        uint8_t want = sf_for_phase(phase);
        if (want != g_sf) {
            radio_set_sf(want);
            g_sf = want;
        }
    }
}

uint8_t adr_current_sf() { return g_sf; }
