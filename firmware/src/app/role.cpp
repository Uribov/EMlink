// ============================================================
// role.cpp —— 角色逻辑（AIR / GCS / RELAY）
// 依赖 nvs_cfg.h / mav_router.h；约定见架构 §1/§2
// ============================================================
#include "role.h"
#include "../service/mav_router.h"

static uint8_t g_role = ROLE_UNSET;

uint8_t role_current() { return g_role; }

void role_apply(uint8_t role) {
    g_role = role;
    // 根据角色确定本机 MAVLink sysid（AIR=飞控号，GCS=255）
    uint8_t sysid = 0;
    if (role == ROLE_AIR) sysid = MAV_SYSID_FC;       // 1
    else if (role == ROLE_GCS) sysid = MAV_SYSID_GCS; // 255
    mav_router_init(sysid);
}
