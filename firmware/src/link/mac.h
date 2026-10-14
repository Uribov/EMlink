#pragma once
// ============================================================
// mac —— 信道接入：LBT(CAD)、ACK/重传、退避
// 依赖：radio.h；约定见 ../../mesh_firmware_architecture.md §5
// ============================================================
#include <Arduino.h>

// 默认参数（可被 NVS 覆盖）
// ACK 超时下限（NVS 默认）；实际超时 = max(本值, ACK 帧空中时间 + MAC_ACK_GUARD_MS)
#define MAC_ACK_TIMEOUT_MS   300
// ACK 帧空中时间之外的余量：收方处理 + 排队 + 射频切换
#define MAC_ACK_GUARD_MS     50
#define MAC_MAX_RETRIES      3
#define MAC_LBT_MAX_TRIES    3
#define MAC_BACKOFF_MIN_MS   5
#define MAC_BACKOFF_MAX_MS   20

typedef enum {
    MAC_OK = 0,
    MAC_ACK_TIMEOUT,   // 重传耗尽仍未收 ACK
    MAC_LBT_BUSY,      // 3 次 LBT 都忙（已硬发）
    MAC_TX_ERR,        // 射频发送失败
    MAC_DROP_RATE,   // 被流量整形丢弃（P1-10：不阻塞、由上层计数）
} mac_result_t;

// 发送一帧：LBT -> 发送 -> (可选)等 ACK -> 退避重传
mac_result_t mac_send(const uint8_t* frame, size_t len, bool ack_req);

// 构造并发送 ACK（回给 src_id，echo 其 seq）
void mac_send_ack(uint8_t src_id, uint16_t seq);

// 更新参数（来自 NVS）
void mac_set_params(uint32_t ack_timeout_ms, uint8_t retries);

// ACK 帧到达通知（mesh_task 收到 FRAME_ACK 时调用）：匹配正在等的 seq 则标记
void mac_on_ack_rx(uint8_t ack_src, uint16_t ack_seq);

// 统计
uint32_t mac_tx_count();
uint32_t mac_ack_ok();
uint32_t mac_ack_miss();
uint32_t mac_retries();

// 启动 MAC 任务（内部 xTaskCreate mac_task_func，任务函数为模块内 static）
void mac_start(void);
