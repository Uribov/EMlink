#pragma once
// ============================================================
// role —— 角色逻辑（AIR / GCS / RELAY）
// 依赖：nvs_cfg.h；约定见 ../../mesh_firmware_architecture.md §1/§2
// ============================================================
#include <Arduino.h>
#include "../service/nvs_cfg.h"

// 根据角色初始化对应数据通路：
//   AIR   -> uart_fc + mesh + mav_router(飞控 sysid)
//   GCS   -> usb_cdc(CDC0 MAVLink) + mesh + mav_router(255)
//   RELAY -> 仅 mesh（无 MAVLink 业务）
void role_apply(uint8_t role);

// 当前角色
uint8_t role_current();
