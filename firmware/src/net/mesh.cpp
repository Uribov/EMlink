// ============================================================
// mesh.cpp —— 泛洪转发、去重、TTL 防环、反向路径单播
// 依赖 frame_codec.h / route.h；约定见架构 §4.3/§4.4/§4.5
// ============================================================
#include "mesh.h"
#include "../rtos.h"
#include "route.h"
#include "../link/mac.h"
#include "../service/nvs_cfg.h"

// ---- 去重表 ----
#define MESH_MAX_SRC 16

struct dedup_slot_t {
    uint8_t  src_id;
    uint16_t last_seq;
    uint32_t bitmap[8];   // 256 bit = 8 × 32
    uint32_t last_seen_ms;
};

static dedup_slot_t g_dedup[MESH_MAX_SRC];
static uint8_t g_my_node_id = 1;
static uint8_t g_my_ttl = FRAME_DEF_TTL;
static uint16_t g_seq = 0;

static TaskHandle_t g_mesh_task = nullptr;
static mesh_rx_cb_t g_rx_cb = nullptr;

// 统计
static uint32_t g_fwd = 0, g_drop_ttl = 0, g_drop_dup = 0, g_drop_crc = 0, g_rx = 0;
static uint32_t g_last_rx_ms = 0;

// ---- 位图工具 ----
static bool bm_get(const uint32_t* bm, int bit) { return (bm[bit >> 5] >> (bit & 31)) & 1; }
static void bm_set(uint32_t* bm, int bit)      { bm[bit >> 5] |= (1u << (bit & 31)); }

static void bm_shift_left(uint32_t* bm, int words, int shift) {
    if (shift <= 0) return;
    if (shift >= words * 32) { memset(bm, 0, words * 4); return; }
    int ws = shift / 32, bs = shift % 32;
    for (int i = words - 1; i >= 0; i--) {
        uint32_t lo = (i - ws >= 0) ? (bm[i - ws] << bs) : 0;
        uint32_t hi = (bs && (i - ws - 1 >= 0)) ? (bm[i - ws - 1] >> (32 - bs)) : 0;
        bm[i] = lo | hi;
    }
}

static dedup_slot_t* dedup_slot(uint8_t src_id) {
    dedup_slot_t* free_slot = nullptr;
    for (int i = 0; i < MESH_MAX_SRC; i++) {
        if (g_dedup[i].src_id == src_id) return &g_dedup[i];
        if (!free_slot && g_dedup[i].src_id == 0) free_slot = &g_dedup[i];
    }
    return free_slot;
}

// 去重检查：返回 true=重复（丢弃）
static bool dedup_check(uint8_t src_id, uint16_t seq) {
    dedup_slot_t* s = dedup_slot(src_id);
    if (!s) return false;   // 表满，不判重（宁重复不丢新帧）

    uint32_t now = millis();
    if (s->src_id == 0 || (now - s->last_seen_ms) > MESH_DEDUP_TTL_MS) {
        s->src_id = src_id;
        s->last_seq = seq;
        memset(s->bitmap, 0, sizeof(s->bitmap));
        bm_set(s->bitmap, 0);
        s->last_seen_ms = now;
        return false;
    }
    s->last_seen_ms = now;

    int16_t diff = (int16_t)(s->last_seq - seq);
    if (diff == 0) return true;
    if (diff > 0 && diff < MESH_DEDUP_WINDOW) {
        if (bm_get(s->bitmap, diff)) return true;
        bm_set(s->bitmap, diff);
        return false;
    }
    if (diff < 0) {
        bm_shift_left(s->bitmap, 8, -diff);
        s->last_seq = seq;
        bm_set(s->bitmap, 0);
        return false;
    }
    return false;   // 太旧（窗口外），忽略
}

void mesh_init() {
    memset(g_dedup, 0, sizeof(g_dedup));
    cfg_t c;
    if (nvs_cfg_load(&c)) {
        if (c.node_id != 0) g_my_node_id = c.node_id;
        if (c.ttl != 0)     g_my_ttl = c.ttl;
    }
}

void mesh_set_rx_cb(mesh_rx_cb_t cb) {
    g_rx_cb = cb;
}

// 去重：命中返回 false（丢弃）
bool mesh_ingress(const frame_t* f) {
    if (!f) return false;
    if (dedup_check(f->src_id, f->seq)) {
        g_drop_dup++;
        return false;
    }
    return true;
}

// 本机发起发送：决定 relay_id（单播查下一跳，未知回退广播）
bool mesh_send(uint8_t dst_id, uint8_t qos, bool ack_req,
               const uint8_t* payload, size_t len, uint8_t orig_sysid) {
    if (!payload || len > FRAME_MAX_PAYLOAD) return false;

    frame_t f = {};
    f.type       = FRAME_DATA;
    f.qos        = qos;
    f.ack_req    = ack_req;
    f.frag       = false;
    f.proto      = PROTO_MAVLINK;
    f.src_id     = g_my_node_id;
    f.dst_id     = dst_id;
    f.relay_id   = 0;
    f.seq        = g_seq++;
    f.ttl        = g_my_ttl;
    f.orig_sysid = orig_sysid;
    f.payload    = payload;
    f.payload_len = len;

    if (dst_id != 0) {
        uint8_t nh = route_next_hop(dst_id);
        if (nh == 0) {
            // 目标路径未知：回退广播
            f.dst_id = 0;
        } else if (nh != dst_id) {
            // 经中继
            f.relay_id = nh;
        }
        // nh == dst_id：目标直连，relay 保持 0
    }

    uint8_t buf[FRAME_MTU];
    size_t n = frame_encode(&f, buf, sizeof(buf));
    if (n == 0) return false;

    queue_item_t item = {};
    memcpy(item.data, buf, n);
    item.len = (uint16_t)n;
    item.qos = qos;
    item.ack_req = ack_req ? 1 : 0;
    return rtos_tx_enqueue(&item);
}

bool mesh_send_beacon() {
    frame_t f = {};
    f.type       = FRAME_BEACON;
    f.qos        = QOS0;
    f.ack_req    = false;
    f.frag       = false;
    f.proto      = PROTO_CUSTOM;   // BEACON 无 MAVLink 载荷
    f.src_id     = g_my_node_id;
    f.dst_id     = 0;              // 广播
    f.relay_id   = 0;
    f.seq        = g_seq++;
    f.ttl        = 1;              // 只一跳，不转发
    f.orig_sysid = 0;
    f.payload    = nullptr;
    f.payload_len = 0;

    uint8_t buf[FRAME_MTU];
    size_t n = frame_encode(&f, buf, sizeof(buf));
    if (n == 0) return false;

    queue_item_t item = {};
    memcpy(item.data, buf, n);
    item.len = (uint16_t)n;
    item.qos = QOS0;
    item.ack_req = 0;
    return rtos_tx_enqueue(&item);
}

uint32_t mesh_fwd_count()      { return g_fwd; }
uint32_t mesh_drop_ttl_count() { return g_drop_ttl; }
uint32_t mesh_drop_dup_count() { return g_drop_dup; }
uint32_t mesh_drop_crc_count() { return g_drop_crc; }
uint32_t mesh_rx_count()       { return g_rx; }
uint32_t mesh_last_rx_ms()     { return g_last_rx_ms; }

// ---- mesh_task：收帧 -> 解码 -> ACK/去重/TTL -> 投递/转发 ----
static void mesh_task_func(void* arg) {
    (void)arg;
    queue_item_t item;
    for (;;) {
        if (xQueueReceive(q_rf_rx, &item, portMAX_DELAY) != pdTRUE) continue;

        frame_t f;
        if (!frame_decode(item.data, item.len, &f)) {
            g_drop_crc++;
            continue;
        }
        g_rx++;
        g_last_rx_ms = millis();

        // ACK 帧：通知 mac
        if (f.type == FRAME_ACK) {
            mac_on_ack_rx(f.src_id, f.seq);
            continue;
        }

        // 去重（重复帧不处理不转发）
        if (!mesh_ingress(&f)) continue;

        // BEACON：记录邻居链路质量，不投递不转发
        if (f.type == FRAME_BEACON) {
            route_learn(f.src_id, 0);
            route_update_rssi(f.src_id, item.rssi);
            continue;
        }

        // 数据帧且要 ACK：回 ACK
        if (f.type == FRAME_DATA && f.ack_req) {
            mac_send_ack(f.src_id, f.seq);
        }

        // 反向路径学习（广播帧：relay_id 即上一跳）
        if (f.dst_id == 0) route_learn(f.src_id, f.relay_id);

        // 投递本机（dst 是自己或广播）
        if (f.type == FRAME_DATA && (f.dst_id == 0 || f.dst_id == g_my_node_id)) {
            if (g_rx_cb) g_rx_cb(&f, (float)item.rssi, (float)item.snr);
        }

        // 转发判定
        if (f.ttl == 0) { g_drop_ttl++; continue; }

        bool fwd = false;
        uint8_t new_dst = f.dst_id;
        uint8_t relay = 0;

        if (f.dst_id == 0) {
            // 广播：人人转发一次
            fwd = true;
            relay = g_my_node_id;
        } else if (f.dst_id != g_my_node_id) {
            // 单播给别人：只有"下一跳=我"才转发
            if (f.relay_id == g_my_node_id) {
                uint8_t nh = route_next_hop(f.dst_id);
                if (nh == 0) {
                    // 目标路径未知：回退广播
                    fwd = true;
                    new_dst = 0;
                    relay = g_my_node_id;
                } else {
                    fwd = true;
                    relay = nh;   // 下一跳
                }
            }
        }

        if (!fwd) continue;

        f.ttl--;
        f.dst_id = new_dst;
        f.relay_id = relay;

        uint8_t out[FRAME_MTU];
        size_t n = frame_encode(&f, out, sizeof(out));
        if (n > 0) {
            queue_item_t tx = {};
            memcpy(tx.data, out, n);
            tx.len = (uint16_t)n;
            tx.qos = f.qos;
            tx.ack_req = f.ack_req ? 1 : 0;   // 保持逐跳 ACK
            rtos_tx_enqueue(&tx);
        }
        g_fwd++;
    }
}

void mesh_start(void) {
    xTaskCreatePinnedToCore(mesh_task_func, "mesh", MESH_TASK_STACK,
                            nullptr, MESH_TASK_PRIO, &g_mesh_task, MESH_TASK_CORE);
}
