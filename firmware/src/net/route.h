#pragma once
// ============================================================
// route —— 反向路径学习 + 路由表（预留）
// 依赖：无；约定见 ../../mesh_firmware_architecture.md §4.5
// ============================================================
#include <Arduino.h>

// 邻居表最大条目
#define ROUTE_MAX_NEIGHBORS 16

// 路径老化：这么久没再听到该源即视为失效（外场按实测调；太短会让单播频繁退回广播）
#define ROUTE_PATH_TTL_MS 120000

// 邻居信息
struct neighbor_t {
    uint8_t  node_id;
    int16_t  last_rssi;
    uint32_t last_seen_ms;
};

void route_init();

// 反向路径学习：记录 src_id 是从哪一跳来的
void route_learn(uint8_t src_id, uint8_t via_neighbor);

// 查 src_id 的上一跳（单播回信用）；无记录返回 0
uint8_t route_next_hop(uint8_t src_id);

// 导出邻居表（给 stats/CLI）；返回条目数
uint8_t route_neighbors(neighbor_t* out, uint8_t max);

// 更新某邻居的 RSSI（收到 BEACON 时调用）
void route_update_rssi(uint8_t src_id, int16_t rssi);

// 因过期/满淘汰而失效的路径条数（诊断用）
uint32_t route_expired_count();
uint32_t route_evict_count();
