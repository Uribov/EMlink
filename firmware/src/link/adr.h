#pragma once
// ============================================================
// adr —— 自适应速率（BEACON 锚定固定速率表，非真动态自适应）
// 方案（方案已定死）：
//   - GCS 每 5s 发 BEACON（固定 SF10 锚点，慢而稳谁都听得见）
//   - 5s 周期：t=0~1s 全网 SF10（慢速保链接），t=1~5s 全网 SF7（快速传数据）
//   - 节点停在 SF10 等 BEACON，收到即对齐周期起点
// 依赖：radio.h（radio_set_sf）；规格：adr.md
// ============================================================
#include <Arduino.h>

// 速率表常量（保守值）
#define ADR_SF_SLOW     10     // 慢速保链接窗的 SF（BEACON 也用这个）
#define ADR_SF_FAST     7      // 快速窗的 SF
#define ADR_PERIOD_MS   5000   // 周期 5s
#define ADR_SLOW_MS     1000   // 周期起点起的慢速窗 1s（含 BEACON）
#define ADR_GUARD_MS    500    // 周期末尾提前 0.5s 回 SF10 等 BEACON（抵消空中传播延迟）

// ---- P1-3 兜底 ----
#define ADR_BEACON_LOST_MS   15000  // 失去对齐基准这么久 → 进兼容模式（3 个周期）
#define ADR_BEACON_QUEUE_MS     50  // 估算的"入队→真正上空"延迟，对齐补偿用（外场可调）
#define ADR_COMPAT_SF        ADR_SF_FAST  // 兼容模式固定速率（TDM 里占比最大的那档）

// 初始化（读默认 SF，先停在 SF10 等 BEACON）
void adr_init();

// 收到 BEACON 帧时调用（mesh_task 里）；relay_id==0 表示本机直收（权威信标），否则多跳转发
void adr_on_beacon(uint8_t src_id, uint8_t relay_id);

// 周期 tick（由 mac_task 循环每轮调一次）；GCS 在周期起点发 BEACON + 切 SF
void adr_tick();

// 当前应该用的 SF（供 CLI/stats 查询）
uint8_t adr_current_sf();

// 诊断（P1-3）
uint32_t adr_beacon_count(void);        // 权威信标数
uint32_t adr_beacon_hop_count(void);    // 多跳信标数（仅"系统在场"可见性）
uint32_t adr_desync_count(void);        // 进入兼容模式的次数
uint32_t adr_multi_anchor_count(void);  // 相位被别的锚点抢的次数
bool     adr_has_beacon(void);          // 是否处于对齐状态
bool     adr_compat_mode(void);         // 是否处于降级兼容模式
