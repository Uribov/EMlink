#pragma once
// ============================================================
// addr_map —— sysid(系统标识) ↔ node_id(节点地址) 对照表
// 依赖：无；约定见 ../../mesh_firmware_architecture.md §7
// ============================================================
#include <Arduino.h>

#define ADDR_MAP_MAX 32

// 心跳 1Hz，60s 没听到即认为该 sysid 已不在网
#define ADDR_ENTRY_TTL_MS 60000

struct addr_entry_t {
    uint8_t  sysid;
    uint8_t  node_id;
    uint32_t last_seen_ms;
};

void addr_map_init();

// 从 HEARTBEAT 学习：sysid 住在哪个 node_id
void addr_map_learn(uint8_t sysid, uint8_t node_id);

// sysid -> node_id；未知返回 0
uint8_t addr_map_node(uint8_t sysid);

// node_id -> sysid；未知返回 0
uint8_t addr_map_sysid(uint8_t node_id);

// 导出表（给 stats/CLI 显示链路质量）；返回条目数
uint8_t addr_map_export(addr_entry_t* out, uint8_t max);

// 因过期/满淘汰而失效的条数（诊断用）
uint32_t addr_map_expired_count();
uint32_t addr_map_evict_count();
