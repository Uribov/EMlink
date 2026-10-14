#pragma once
#include <Arduino.h>
// ============================================================
// role_state —— 本机角色的唯一真源（基础支撑，跨层共享）
// ============================================================
// 为什么单独放这里：中间层（link/adr、service/ota）需要知道"本机是不是 GCS"，
// 但按分层理念，中间层不允许依赖应用层（app/role）。角色值是各层的共同词汇，
// 所以把"当前角色"这个状态放在基础支撑里：
//   应用层（app/role）= 决定角色（读 NVS、按角色选 MAVLink sysid）
//   中间层            = 只读角色
// 角色常量 ROLE_* 定义在 service/nvs_cfg.h。
uint8_t role_state_get(void);          // 读当前角色（未设置时 = ROLE_UNSET）
void    role_state_set(uint8_t role);  // 由 app/role 的 role_apply() 调用
