#pragma once
// ============================================================
// route —— 反向路径学习 + 路由表（预留）
// 依赖：无；约定见 ../../mesh_firmware_architecture.md §4.5
// ============================================================
#include <Arduino.h>

// 邻居表最大条目
#define ROUTE_MAX_NEIGHBORS 16

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
