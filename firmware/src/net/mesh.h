#pragma once
// ============================================================
// mesh —— 泛洪转发、去重、TTL 防环
// 依赖：frame_codec.h；约定见 ../../mesh_firmware_architecture.md §4.3/§4.4/§4.5
// ============================================================
#include <Arduino.h>
#include "../link/frame_codec.h"

// 去重窗（每源 256 序号槽）+ 槽位超时
#define MESH_DEDUP_WINDOW   256
#define MESH_DEDUP_TTL_MS   30000

// 初始化去重表
void mesh_init();

// 收到一帧后处理：去重 -> TTL -> 判定（转发 / 丢弃 / 投递本机）
// 返回 true=应转发出（mesh_task 调 mac_send），false=丢弃
bool mesh_ingress(const frame_t* f);

// 本机收到数据帧时的回调（投给 mav_router / frag）
typedef void (*mesh_rx_cb_t)(const frame_t* f, float rssi, float snr);
void mesh_set_rx_cb(mesh_rx_cb_t cb);

// 本机发起发送：填 seq/ttl，交 mac
bool mesh_send(uint8_t dst_id, uint8_t qos, bool ack_req,
               const uint8_t* payload, size_t len, uint8_t orig_sysid);

// 分配发送序号（跨核原子，portMUX 保护）。全系统唯一 seq 源，
// 普通帧/分片帧/控制帧都从这里取，避免多计数器 seq 重叠导致去重误判。
uint16_t mesh_next_seq();

// 发控制帧（FRAME_NACK / FRAME_CFG 等）：type 自选，无 MAVLink 载荷
// payload 走加密；单播同 mesh_send 的 relay 逻辑
bool mesh_send_ctrl(uint8_t type, uint8_t dst_id, uint8_t qos, bool ack_req,
                    const uint8_t* payload, size_t len);

// GCS 广播 BEACON（邻居据此评估链路质量，架构 §5）
bool mesh_send_beacon();

// ping 探测：发 FRAME_PING 帧测 RTT。返回 rtt_ms（<0 = 超时/失败）；
// rssi_out（可空）输出对方信号。阻塞等待，最长 timeout_ms。
int32_t mesh_ping(uint8_t dst_id, uint32_t timeout_ms, int16_t* rssi_out);

// 统计
uint32_t mesh_fwd_count();
uint32_t mesh_drop_ttl_count();
uint32_t mesh_drop_dup_count();
uint32_t mesh_drop_crc_count();
uint32_t mesh_rx_count();
uint32_t mesh_last_rx_ms();   // 最后收帧时间 ms（失联判定用）

// 撞号冲突计数（收到"自称本机 node_id"且 seq 不在本机已发序号里的陌生帧）
uint32_t mesh_id_conflict_count();

// 启动 mesh 任务（内部 xTaskCreate mesh_task_func，任务函数为模块内 static）
void mesh_start(void);
