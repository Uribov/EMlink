// ============================================================
// adr.cpp —— BEACON 锚定固定速率表
// 依赖 radio.h / mesh.h（发 BEACON）/ role_state.h（判断 GCS）
// 5s 周期：0~1s SF9（慢速窗），1~5s SF7（快速窗）。
// GCS 在周期起点发 BEACON（SF9）；非 GCS 停在 SF9 等 BEACON，收到后对齐。
// ============================================================
#include "adr.h"
#include "../drivers/radio.h"
#include "../net/mesh.h"
#include "../service/role_state.h"
#include "../service/nvs_cfg.h"
#include "../link/frame_codec.h"

static volatile bool     g_adr_enabled = true; // ADR 总开关（adr_init 从 NVS adr_en 读）
static volatile uint8_t  g_cfg_sf = ADR_SF_FAST; // NVS 配置的 SF（ADR 关闭时固定用它）
static volatile uint8_t  g_sf = ADR_SF_SLOW;   // 当前 SF（默认先停慢速等 BEACON）
static volatile bool     g_has_beacon = false; // 是否已对齐到周期起点
static volatile uint32_t g_epoch_ms  = 0;      // 本周期起点（对齐后有效）
static volatile uint32_t g_last_beacon_ms = 0; // 上次收到权威 BEACON 时刻（对齐基准）

// ---- P1-3 兜底状态 ----
static volatile uint32_t g_beacon_count = 0;        // 权威信标数
static volatile uint32_t g_hop_count = 0;           // 多跳信标数
static volatile uint32_t g_desync = 0;              // 进入兼容模式次数
static volatile uint32_t g_multi_anchor = 0;        // 相位被别的锚点抢的次数
static volatile bool     g_compat = false;          // 是否处于降级兼容模式
static volatile uint32_t g_last_any_beacon_ms = 0;  // 最近一次收到任意信标（含多跳）
static volatile uint32_t g_first_any_beacon_ms = 0; // 第一次收到任意信标的时刻
static volatile uint8_t  g_anchor_src_id = 0;       // 当前锚点 node_id，0=未知
static volatile uint32_t g_air_seen = 0;            // 收到 AIR 在场信标的次数（不参与多跳语义）

// ---- AIR 低频在场信标（独立于 ADR 开关）----
static volatile uint32_t g_air_beacon_s = 10;        // 间隔秒数（NVS air_beacon_s，0=关闭）
static volatile uint32_t g_last_air_beacon_ms = 0;   // 上次发送时刻
static volatile uint32_t g_air_beacon_count = 0;     // 已发出的 AIR 信标数

void adr_set_enabled(bool on) { g_adr_enabled = on; }
bool adr_is_enabled(void)     { return g_adr_enabled; }

void adr_init() {
    g_has_beacon = false;
    g_epoch_ms = 0;
    g_last_beacon_ms = 0;
    g_beacon_count = 0;
    g_hop_count = 0;
    g_desync = 0;
    g_multi_anchor = 0;
    g_compat = false;
    g_last_any_beacon_ms = 0;
    g_first_any_beacon_ms = 0;
    g_anchor_src_id = 0;
    g_last_air_beacon_ms = millis();   // 开机后先等一个间隔再发，避免启动瞬间抢空口
    g_air_beacon_count = 0;

    // 读 NVS（load 内部对 adr_en / SF×BW 组合做了兜底），决定是否参与 ADR
    cfg_t cfg;
    nvs_cfg_load(&cfg);
    g_cfg_sf = cfg.sf;
    g_adr_enabled = (cfg.adr_en != 0);
    g_air_beacon_s = cfg.air_beacon_s;

    if (g_adr_enabled) {
        g_sf = ADR_SF_SLOW;
        radio_set_sf(ADR_SF_SLOW);   // 开机先停 SF9，保证能收到第一个 BEACON
    } else {
        g_sf = g_cfg_sf;             // 关闭 ADR：保持 radio_init 已配置的 SF，不等 BEACON
    }
}

// 收到 BEACON：relay_id==0 为本机直收（权威信标，参与对齐）；否则只算"系统在场"
void adr_on_beacon(uint8_t src_id, uint8_t relay_id, uint8_t sender_role) {
    // 非 GCS 发的信标只是"在场宣告"，不参与任何时间对齐（放在 ADR 开关判断之前：
    // 即使本机 adr=off 也要能看见飞机还活着）
    if (sender_role != ROLE_GCS) {
        uint32_t t = millis();
        g_last_any_beacon_ms = t;
        if (g_first_any_beacon_ms == 0) g_first_any_beacon_ms = t;
        // 单独计数：AIR 在场信标不是"多跳"，别污染 g_hop_count 的语义
        g_air_seen++;
        return;
    }

    if (!g_adr_enabled) return;   // 关闭 ADR：忽略对端信标，不跟着改 SF

    uint32_t now = millis();
    g_last_any_beacon_ms = now;
    if (g_first_any_beacon_ms == 0) g_first_any_beacon_ms = now;

    if (relay_id != 0) {
        // 多跳信标：中继的排队+空中时延无法补偿，不用于时间对齐，只说明"系统在场"
        g_hop_count++;
        return;
    }

    // 权威信标：本机直收
    g_beacon_count++;

    if (role_state_get() == ROLE_GCS) {
        // GCS 自己就是锚点：不跟随别的锚点（否则两个 GCS 互相把相位拉来拉去）
        if (g_anchor_src_id != 0 && g_anchor_src_id != src_id) g_multi_anchor++;
        return;
    }

    if (g_has_beacon && g_anchor_src_id != src_id &&
        (uint32_t)(now - g_epoch_ms) < ADR_PERIOD_MS / 2) {
        g_multi_anchor++;   // 半周期内又冒出一个锚点 → 相位被抢（选举留待后续）
    }

    g_anchor_src_id = src_id;
    g_last_beacon_ms = now;
    g_compat = false;                    // 收到权威信标 → 退出兼容模式（自愈）

    // 对齐点补偿：GCS 的时间戳是"入队"时刻，本机基准是"收完"时刻，
    // 差 ≈ 信标空中时间 + 入队排队 → 减掉它，误差不再侵占 500ms guard 窗
    uint32_t toa = radio_airtime_ms(FRAME_HDR_LEN + FRAME_CRC_LEN);
    g_epoch_ms = now - (toa + ADR_BEACON_QUEUE_MS);
    g_has_beacon = true;
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
    // 周期末尾 guard 窗：提前回 SF9 等下一个 BEACON（抵消空中传播延迟 ~200ms）
    if (phase >= ADR_PERIOD_MS - ADR_GUARD_MS) return ADR_SF_SLOW;
    // 其余时间快速窗 SF7
    return ADR_SF_FAST;
}

void adr_tick() {
    if (!g_adr_enabled) return;   // 关闭 ADR：不切 SF、不发 BEACON、不做相位协商

    uint32_t now = millis();
    uint8_t role = role_state_get();

    // P1-3 失步检测（只对非 GCS）：
    //  · 曾对齐过 → 权威信标超过 ADR_BEACON_LOST_MS 没来
    //  · 从未对齐 → 见过信标（含多跳，说明本机离锚点超过 1 跳）却始终没等到权威信标
    // 两种情况都说明"对齐基准没了/拿不到"：硬撑 TDM 只会丢包，降级到固定速率更好
    if (role != ROLE_GCS && !g_compat) {
        bool lost = false;
        if (g_has_beacon)         lost = (uint32_t)(now - g_last_beacon_ms) > ADR_BEACON_LOST_MS;
        // 收到过任意信标（多跳中转 或 同级 AIR 在场）却始终没等到权威信标 → 说明拿不到锚点
        else if (g_hop_count > 0 || g_air_seen > 0)
            lost = (uint32_t)(now - g_first_any_beacon_ms) > ADR_BEACON_LOST_MS;
        if (lost) {
            g_compat = true;
            g_desync++;
            Serial.printf("[adr] 失去对齐基准（%s）→ 进入兼容模式：固定 SF%u，不再切窗\n",
                          g_has_beacon ? "权威信标超时" : "离锚点超过 1 跳",
                          (uint8_t)ADR_COMPAT_SF);
        }
    }

    if (role == ROLE_GCS) {
        // GCS：自己就是锚点，5s 一周期，起点发 BEACON（SF9）
        if (now - g_last_beacon_ms >= ADR_PERIOD_MS) {
            g_last_beacon_ms = now;
            g_epoch_ms = now;
            g_has_beacon = true;

            // 切到 SF9 再发 BEACON（保证收方在 SF9 能听到）
            if (g_sf != ADR_SF_SLOW) {
                radio_set_sf(ADR_SF_SLOW);
                g_sf = ADR_SF_SLOW;
            }
            mesh_send_beacon(ROLE_GCS);
            return;   // 本 tick 就停 SF9，下个 tick 再按相位切
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
        // 非 GCS（AIR/RELAY）：停在 SF9 等 BEACON；收到后按相位切
        if (g_compat) {
            // 兼容模式：固定速率，不切窗（已经失去对齐基准）
            if (g_sf != ADR_COMPAT_SF) { radio_set_sf(ADR_COMPAT_SF); g_sf = ADR_COMPAT_SF; }
            return;
        }

        if (!g_has_beacon) {
            // 还没收到过 BEACON，保持 SF9 傻等
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

// AIR 低频在场信标：不走 adr_tick（那个函数在 adr=off 时直接 return），
// 用独立 millis 计时；不切 SF，使用当前速率发送。
void air_beacon_tick() {
    if (role_state_get() != ROLE_AIR) return;
    if (g_air_beacon_s == 0) return;   // 0 = 关闭

    uint32_t now = millis();
    if ((uint32_t)(now - g_last_air_beacon_ms) < g_air_beacon_s * 1000) return;

    g_last_air_beacon_ms = now;        // 先记时刻：即使入队失败也不每 tick 重试
    if (mesh_send_beacon(ROLE_AIR)) g_air_beacon_count++;
}

uint8_t adr_current_sf() { return g_adr_enabled ? g_sf : g_cfg_sf; }

uint32_t adr_beacon_count()       { return g_beacon_count; }
uint32_t adr_beacon_hop_count()   { return g_hop_count; }
uint32_t adr_desync_count()       { return g_desync; }
uint32_t adr_multi_anchor_count() { return g_multi_anchor; }
uint32_t adr_air_beacon_count()   { return g_air_beacon_count; }
uint32_t adr_air_seen_count()     { return g_air_seen; }
bool     adr_has_beacon()         { return g_has_beacon; }
bool     adr_compat_mode()        { return g_compat; }
