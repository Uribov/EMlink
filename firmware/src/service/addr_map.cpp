// ============================================================
// addr_map.cpp —— sysid ↔ node_id 对照表（从 HEARTBEAT 学习）
// ============================================================
#include "addr_map.h"
#include <string.h>

static addr_entry_t g_map[ADDR_MAP_MAX];
static uint8_t g_n = 0;
static uint32_t g_expired = 0, g_evict = 0;
static portMUX_TYPE g_addr_mux = portMUX_INITIALIZER_UNLOCKED;   // 跨核保护 g_map

void addr_map_init() {
    portENTER_CRITICAL(&g_addr_mux);
    g_n = 0;
    memset(g_map, 0, sizeof(g_map));
    portEXIT_CRITICAL(&g_addr_mux);
}

// 丢掉已过期的条目（压缩数组），返回是否仍满
// （调用时必须已在 g_addr_mux 临界区内）
static bool addr_purge_expired_locked(uint32_t now) {
    for (int i = 0; i < (int)g_n; ) {
        if (now - g_map[i].last_seen_ms > ADDR_ENTRY_TTL_MS) {
            g_map[i] = g_map[--g_n];   // 尾部搬过来
            memset(&g_map[g_n], 0, sizeof(addr_entry_t));
            g_expired++;
        } else i++;
    }
    return g_n >= ADDR_MAP_MAX;
}

void addr_map_learn(uint8_t sysid, uint8_t node_id) {
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n; i++) {
        if (g_map[i].sysid == sysid) {
            g_map[i].node_id = node_id;
            g_map[i].last_seen_ms = millis();
            portEXIT_CRITICAL(&g_addr_mux);
            return;
        }
    }
    if (addr_purge_expired_locked(millis())) {
        // 表仍满：淘汰 last_seen_ms 最旧的条目
        int oldest = 0;
        for (int i = 1; i < g_n; i++) {
            if ((int32_t)(g_map[i].last_seen_ms - g_map[oldest].last_seen_ms) < 0)
                oldest = i;
        }
        g_map[oldest].sysid        = sysid;
        g_map[oldest].node_id      = node_id;
        g_map[oldest].last_seen_ms = millis();
        g_evict++;
    } else {
        g_map[g_n].sysid        = sysid;
        g_map[g_n].node_id      = node_id;
        g_map[g_n].last_seen_ms = millis();
        g_n++;
    }
    portEXIT_CRITICAL(&g_addr_mux);
}

uint8_t addr_map_node(uint8_t sysid) {
    uint8_t r = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n; i++) {
        if (g_map[i].sysid == sysid) {
            if (now - g_map[i].last_seen_ms > ADDR_ENTRY_TTL_MS) {
                // 过期：避免把包单播给已离网的 node_id
                g_expired++;
                g_map[i].sysid = 0;
                break;
            }
            r = g_map[i].node_id;
            break;
        }
    }
    portEXIT_CRITICAL(&g_addr_mux);
    return r;
}

uint8_t addr_map_sysid(uint8_t node_id) {
    uint8_t r = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n; i++) {
        if (g_map[i].node_id == node_id) {
            if (now - g_map[i].last_seen_ms > ADDR_ENTRY_TTL_MS) {
                g_expired++;
                g_map[i].node_id = 0;
                break;
            }
            r = g_map[i].sysid;
            break;
        }
    }
    portEXIT_CRITICAL(&g_addr_mux);
    return r;
}

uint8_t addr_map_export(addr_entry_t* out, uint8_t max) {
    uint8_t n = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n && n < max; i++) {
        if (now - g_map[i].last_seen_ms > ADDR_ENTRY_TTL_MS) continue;  // 跳过过期条目
        out[n++] = g_map[i];
    }
    portEXIT_CRITICAL(&g_addr_mux);
    return n;
}

uint32_t addr_map_expired_count() { return g_expired; }
uint32_t addr_map_evict_count()   { return g_evict; }
