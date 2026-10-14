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
static uint32_t g_expired = 0, g_evict = 0;
static portMUX_TYPE g_route_mux = portMUX_INITIALIZER_UNLOCKED;   // 跨核保护 g_paths

void route_init() {
    portENTER_CRITICAL(&g_route_mux);
    g_n = 0;
    memset(g_paths, 0, sizeof(g_paths));
    portEXIT_CRITICAL(&g_route_mux);
}

// 丢掉已过期的条目（压缩数组），返回是否仍满
// （调用时必须已在 g_route_mux 临界区内）
static bool route_purge_expired_locked(uint32_t now) {
    for (int i = 0; i < (int)g_n; ) {
        if (now - g_paths[i].seen_ms > ROUTE_PATH_TTL_MS) {
            g_paths[i] = g_paths[--g_n];   // 尾部搬过来
            memset(&g_paths[g_n], 0, sizeof(path_t));
            g_expired++;
        } else i++;
    }
    return g_n >= ROUTE_MAX_NEIGHBORS;
}

void route_learn(uint8_t src_id, uint8_t via_neighbor) {
    // via_neighbor=0 表示 src 直连（下一跳就是 src 自己）
    uint8_t next = (via_neighbor == 0) ? src_id : via_neighbor;
    portENTER_CRITICAL(&g_route_mux);
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) {
            g_paths[i].via = next;
            g_paths[i].seen_ms = millis();
            portEXIT_CRITICAL(&g_route_mux);
            return;
        }
    }
    if (route_purge_expired_locked(millis())) {
        // 表仍满：淘汰 seen_ms 最旧的条目
        int oldest = 0;
        for (int i = 1; i < g_n; i++) {
            if ((int32_t)(g_paths[i].seen_ms - g_paths[oldest].seen_ms) < 0)
                oldest = i;
        }
        g_paths[oldest].src_id  = src_id;
        g_paths[oldest].via     = next;
        g_paths[oldest].rssi    = 0;
        g_paths[oldest].seen_ms = millis();
        g_evict++;
    } else {
        g_paths[g_n].src_id  = src_id;
        g_paths[g_n].via     = next;
        g_paths[g_n].rssi    = 0;
        g_paths[g_n].seen_ms = millis();
        g_n++;
    }
    portEXIT_CRITICAL(&g_route_mux);
}

uint8_t route_next_hop(uint8_t src_id) {
    portENTER_CRITICAL(&g_route_mux);
    uint32_t now = millis();
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) {
            if (now - g_paths[i].seen_ms > ROUTE_PATH_TTL_MS) {
                // 过期：陈旧下一跳会把单播发进死胡同；置空并返回 0（未知 → 上层回退广播）
                g_expired++;
                g_paths[i].src_id = 0;
                portEXIT_CRITICAL(&g_route_mux);
                return 0;
            }
            uint8_t v = g_paths[i].via;
            portEXIT_CRITICAL(&g_route_mux);
            return v;
        }
    }
    portEXIT_CRITICAL(&g_route_mux);
    return 0;
}

uint8_t route_neighbors(neighbor_t* out, uint8_t max) {
    uint8_t n = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&g_route_mux);
    for (int i = 0; i < g_n && n < max; i++) {
        if (now - g_paths[i].seen_ms > ROUTE_PATH_TTL_MS) continue;  // 跳过陈旧邻居
        out[n].node_id      = g_paths[i].src_id;
        out[n].last_rssi    = g_paths[i].rssi;
        out[n].last_seen_ms = g_paths[i].seen_ms;
        n++;
    }
    portEXIT_CRITICAL(&g_route_mux);
    return n;
}

void route_update_rssi(uint8_t src_id, int16_t rssi) {
    portENTER_CRITICAL(&g_route_mux);
    for (int i = 0; i < g_n; i++) {
        if (g_paths[i].src_id == src_id) {
            g_paths[i].rssi = rssi;
            break;
        }
    }
    portEXIT_CRITICAL(&g_route_mux);
}

uint32_t route_expired_count() { return g_expired; }
uint32_t route_evict_count()   { return g_evict; }
