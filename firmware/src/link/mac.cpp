// ============================================================
// mac.cpp —— 信道接入：LBT(CAD)、ACK/重传、退避
// 依赖 radio.h / frame_codec.h / duty.h；约定见架构 §5
// ============================================================
#include "mac.h"
#include "../rtos.h"
#include "frame_codec.h"
#include "adr.h"
#include "../drivers/radio.h"
#include "duty.h"
#include "../service/nvs_cfg.h"

static TaskHandle_t g_mac_task = nullptr;
static uint32_t g_ack_timeout_ms = MAC_ACK_TIMEOUT_MS;
static uint8_t  g_retries = MAC_MAX_RETRIES;
static uint8_t  g_node_id = 1;   // 本机节点地址（mac_start 时从 NVS 读）

// ACK 等待状态（mac_send 阻塞期间，mesh_task 经 mac_on_ack_rx 置位）
static volatile bool     g_waiting_ack = false;
static volatile bool     g_ack_arrived = false;
static volatile uint16_t g_wait_seq = 0;

// 统计
static uint32_t g_ack_ok = 0, g_ack_miss = 0, g_retry = 0, g_tx = 0;

void mac_set_params(uint32_t ack_timeout_ms, uint8_t retries) {
    g_ack_timeout_ms = ack_timeout_ms;
    g_retries = retries;
}

void mac_on_ack_rx(uint8_t ack_src, uint16_t ack_seq) {
    (void)ack_src;
    if (g_waiting_ack && ack_seq == g_wait_seq) {
        g_ack_arrived = true;
    }
}

// LBT：最多 3 次，忙则随机退避 5~20ms；3 次仍忙返回 false（硬发）
static bool mac_lbt() {
    for (int i = 0; i < MAC_LBT_MAX_TRIES; i++) {
        int16_t act = radio_channelActivity();
        if (act <= 0) return true;   // 0=空闲；负=CAD 错误，别卡死，直接发
        uint32_t wait = MAC_BACKOFF_MIN_MS + random(MAC_BACKOFF_MAX_MS - MAC_BACKOFF_MIN_MS + 1);
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
    return false;
}

mac_result_t mac_send(const uint8_t* frame, size_t len, bool ack_req) {
    if (!frame || len == 0) return MAC_TX_ERR;

    // 占空比限流（粗略 airtime：按 len 比例估算，200B@SF7/BW125≈60ms）
    uint32_t airtime = 5 + (uint32_t)len * 300 / 1000;
    while (!duty_reserve(airtime)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    g_tx++;

    // 解析 seq（线上字节 offset 4~5 小端）
    uint16_t my_seq = (uint16_t)(frame[4] | (frame[5] << 8));

    int attempts = ack_req ? (int)g_retries + 1 : 1;
    uint32_t backoff_ms = 100;   // 100 -> 300 -> 700

    bool lbt_ok = false;
    for (int i = 0; i < attempts; i++) {
        lbt_ok = mac_lbt();

        int16_t st = radio_transmit(frame, len, 1000);
        if (st != 0) return MAC_TX_ERR;

        if (!ack_req) break;      // 不需 ACK，发完即走

        // 等 ACK（轮询，避免任务通知绑定特定调用者）
        g_waiting_ack = true;
        g_wait_seq = my_seq;
        g_ack_arrived = false;
        uint32_t start = millis();
        while (!g_ack_arrived && (millis() - start < g_ack_timeout_ms)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        g_waiting_ack = false;

        if (g_ack_arrived) {
            g_ack_ok++;
            return MAC_OK;
        }

        g_ack_miss++;
        g_retry++;
        if (i < attempts - 1) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms == 100) ? 300 : 700;
        }
    }

    if (ack_req) return MAC_ACK_TIMEOUT;
    return lbt_ok ? MAC_OK : MAC_LBT_BUSY;
}

bool mac_should_ack(const uint8_t* frame, size_t len) {
    if (len < 6) return false;
    if ((frame[0] & 0x0F) != FRAME_DATA) return false;   // 只有数据帧要 ACK
    return (frame[1] & FLAG_ACK_REQ) != 0;
}

void mac_send_ack(uint8_t src_id, uint16_t seq) {
    uint8_t buf[FRAME_MTU];
    frame_t f = {};
    f.type    = FRAME_ACK;
    f.qos     = QOS1;
    f.ack_req = false;         // ACK 本身不重传，防 ACK 风暴
    f.src_id  = g_node_id;     // 本机
    f.dst_id  = src_id;        // 回给原发送者
    f.seq     = seq;           // echo 原 seq
    f.ttl     = 1;
    f.proto   = PROTO_MAVLINK;
    size_t n = frame_encode(&f, buf, sizeof(buf));
    if (n > 0) radio_transmit(buf, n, 500);
}

uint32_t mac_tx_count()   { return g_tx; }
uint32_t mac_ack_ok()     { return g_ack_ok; }
uint32_t mac_ack_miss()   { return g_ack_miss; }
uint32_t mac_retries()    { return g_retry; }

// ---- mac_task：从 q_mac_tx 读待发帧，mac_send 发送 ----
static void mac_task_func(void* arg) {
    (void)arg;
    queue_item_t item;
    for (;;) {
        // ADR 周期 tick（与收发同任务串行，切 SF 不会与 SPI 冲突）
        adr_tick();

        // 按优先级读：hi > med > lo（高优先级帧最多延迟 5ms）
        BaseType_t got = xQueueReceive(q_mac_tx_hi, &item, 0);
        if (got != pdTRUE) got = xQueueReceive(q_mac_tx_med, &item, 0);
        if (got != pdTRUE) got = xQueueReceive(q_mac_tx_lo, &item, pdMS_TO_TICKS(5));
        if (got == pdTRUE) {
            mac_send(item.data, item.len, item.ack_req != 0);
        }
    }
}

void mac_start(void) {
    cfg_t c;
    if (nvs_cfg_load(&c) && c.node_id != 0) g_node_id = c.node_id;
    xTaskCreatePinnedToCore(mac_task_func, "mac", MAC_TASK_STACK,
                            nullptr, MAC_TASK_PRIO, &g_mac_task, MAC_TASK_CORE);
}
