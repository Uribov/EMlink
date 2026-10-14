// ============================================================
// role_state.cpp —— 本机角色的唯一真源（基础支撑，跨层共享）
// 只在启动 / 配置变更时由 app/role 的 role_apply() 写入，
// 其余多任务只读；与本工程其它状态一致，无需加锁。
// ============================================================
#include "role_state.h"
#include "nvs_cfg.h"

static uint8_t g_role = ROLE_UNSET;

uint8_t role_state_get() { return g_role; }

void role_state_set(uint8_t role) { g_role = role; }
