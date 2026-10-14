// ============================================================
// route.cpp —— 反向路径学习 + 邻居表（预留路由表位）
// ============================================================
#include "route.h"
#include <string.h>

struct path_t {
    uint8_t  src_id;
    uint8_t  via;
    int16_t  rssi;
    uint32_t seen_ms;
};

static path_t g_paths[ROUTE_MAX_NEIGHBORS];
static uint8_t g_n = 0;

void route_init() {
    g_n = 0;
    memset(g_paths, 0, sizeof(g_paths));
}

void route_learn(uint8_t src_id, uint8_t via_neighbor) {
    // via_neighbor=0 表示 src 直连（下一跳就是 src 自己）
    uint8_t next = (via_neighbor == 0) ? src_id : via_neighbor;
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) {
            g_paths[i].via = next;
            g_paths[i].seen_ms = millis();
            return;
        }
    }
    if (g_n < ROUTE_MAX_NEIGHBORS) {
        g_paths[g_n].src_id  = src_id;
        g_paths[g_n].via     = next;
        g_paths[g_n].rssi    = 0;
        g_paths[g_n].seen_ms = millis();
        g_n++;
    }
}

uint8_t route_next_hop(uint8_t src_id) {
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) return g_paths[i].via;
    }
    return 0;
}

uint8_t route_neighbors(neighbor_t* out, uint8_t max) {
    uint8_t n = 0;
    for (int i = 0; i < g_n && n < max; i++) {
        out[n].node_id      = g_paths[i].src_id;
        out[n].last_rssi    = g_paths[i].rssi;
        out[n].last_seen_ms = g_paths[i].seen_ms;
        n++;
    }
    return n;
}

void route_update_rssi(uint8_t src_id, int16_t rssi) {
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) {
            g_paths[i].rssi = rssi;
            return;
        }
    }
}
