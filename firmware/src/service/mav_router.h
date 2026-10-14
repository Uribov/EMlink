#pragma once
// ============================================================
// mav_router —— MAVLink 2 解析、按 target_sysid 分发、心跳优先
// 只支持 MAVLink 2（STX=0xFD），不支持 v1
// 依赖：frag.h、addr_map.h；约定见 ../../mesh_firmware_architecture.md §7/§8.1
// ============================================================
#include <Arduino.h>

// MAVLink 2 帧常量
#define MAVLINK2_STX         0xFD
#define MAVLINK2_HEADER_LEN  10     // STX..MSGID(3B)，不含 payload/CRC
#define MAVLINK2_CRC_LEN     2
#define MAVLINK2_SIGN_LEN    13     // 签名长度（本项目不用，签名位置位则丢弃）

// MAVLink 惯例（ArduPilot / PX4）
#define MAV_SYSID_FC         1
#define MAV_SYSID_GCS        255
#define MAV_MSGID_HEARTBEAT  0

// 初始化（绑定本机角色 sysid）
void mav_router_init(uint8_t local_sysid);

// 喂入一段 MAVLink 字节流（来自 q_uart_rx / q_usb_rx / mesh_rx_cb）
// 内部按帧切分（0xFD + LEN + CRC），逐条调 mav_router_handle_msg
void mav_router_feed(const uint8_t* data, size_t len);

// 处理一条完整 MAVLink 2 消息：按 payload 里的 target_sysid 决定「送本地串口」或「转 LoRa」
void mav_router_handle_msg(const uint8_t* msg, size_t len);

// 心跳是否超时（配合 failsafe）
bool mav_router_heartbeat_ok();

// 收到 NACK（frag 缺件催发）：从发送缓存补发缺失分片
// payload = [orig_sysid, frag_seq, 缺失位图]
void mav_router_on_nack(const uint8_t* payload, size_t len);

// 启动 MAVLink 路由任务（内部 xTaskCreate mavlink_task_func，任务函数为模块内 static）
void mav_router_start(void);
