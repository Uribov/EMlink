// ============================================================
// role.cpp —— 角色逻辑（AIR / GCS / RELAY）
// 依赖 nvs_cfg.h / mav_router.h；约定见架构 §1/§2
// ============================================================
#include "role.h"
#include "../service/mav_router.h"
#include "../service/role_state.h"

uint8_t role_current() { return role_state_get(); }

void role_apply(uint8_t role) {
    role_state_set(role);
    // 本机 MAVLink sysid：优先用 NVS 配置（sysid=0 表示"按角色默认"），多机靠它区分
    cfg_t c;
    uint8_t sysid = 0;
    if (nvs_cfg_load(&c) && c.sysid != 0) {
        sysid = c.sysid;
    } else if (role == ROLE_AIR) {
        sysid = MAV_SYSID_FC;       // 1
    } else if (role == ROLE_GCS) {
        sysid = MAV_SYSID_GCS;      // 255
    }
    // ROLE_RELAY：保持 0（无 MAVLink 业务）
    mav_router_init(sysid);
}
