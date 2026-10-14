// ============================================================
// addr_map.cpp —— sysid ↔ node_id 对照表（从 HEARTBEAT 学习）
// ============================================================
#include "addr_map.h"
#include <string.h>

static addr_entry_t g_map[ADDR_MAP_MAX];
static uint8_t g_n = 0;
static portMUX_TYPE g_addr_mux = portMUX_INITIALIZER_UNLOCKED;   // 跨核保护 g_map

void addr_map_init() {
    portENTER_CRITICAL(&g_addr_mux);
    g_n = 0;
    memset(g_map, 0, sizeof(g_map));
    portEXIT_CRITICAL(&g_addr_mux);
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
    if (g_n < ADDR_MAP_MAX) {
        g_map[g_n].sysid        = sysid;
        g_map[g_n].node_id      = node_id;
        g_map[g_n].last_seen_ms = millis();
        g_n++;
    }
    portEXIT_CRITICAL(&g_addr_mux);
}

uint8_t addr_map_node(uint8_t sysid) {
    uint8_t r = 0;
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n; i++)
        if (g_map[i].sysid == sysid) { r = g_map[i].node_id; break; }
    portEXIT_CRITICAL(&g_addr_mux);
    return r;
}

uint8_t addr_map_sysid(uint8_t node_id) {
    uint8_t r = 0;
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n; i++)
        if (g_map[i].node_id == node_id) { r = g_map[i].sysid; break; }
    portEXIT_CRITICAL(&g_addr_mux);
    return r;
}

uint8_t addr_map_export(addr_entry_t* out, uint8_t max) {
    uint8_t n = 0;
    portENTER_CRITICAL(&g_addr_mux);
    for (int i = 0; i < g_n && n < max; i++)
        out[n++] = g_map[i];
    portEXIT_CRITICAL(&g_addr_mux);
    return n;
}
