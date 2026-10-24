// ============================================================
// mesh.cpp —— 泛洪转发、去重、TTL 防环、反向路径单播
// 依赖 frame_codec.h / route.h；约定见架构 §4.3/§4.4/§4.5
// ============================================================
#include "mesh.h"
#include "../rtos.h"
#include "route.h"
#include "../link/mac.h"
#include "../link/adr.h"
#include "../service/nvs_cfg.h"
#include "../service/crypto.h"
#include "../service/frag.h"
#include "../service/watchdog.h"
#include "../service/version.h"

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
static portMUX_TYPE g_seq_mux = portMUX_INITIALIZER_UNLOCKED;   // 跨核保护 g_seq

// ---- 发送序号历史环（撞号检测用）----
// 记录本机最近发出的 seq；收到"自称本机 node_id"但 seq 不在此环里的帧 → 判定撞号。
#define MESH_SEQ_HISTORY 32
static uint16_t g_my_seq_hist[MESH_SEQ_HISTORY];
static uint8_t  g_my_seq_idx;
static uint32_t g_id_conflict;

static TaskHandle_t g_mesh_task = nullptr;
static mesh_rx_cb_t g_rx_cb = nullptr;

// 统计
static uint32_t g_fwd = 0, g_drop_ttl = 0, g_drop_dup = 0, g_drop_crc = 0, g_rx = 0;
// 去重表满淘汰次数（>0 说明同场活跃源超过 MESH_MAX_SRC）
static uint32_t g_dedup_evict = 0;
static uint32_t g_drop_auth = 0;
static uint32_t g_last_rx_ms = 0;

// 数据帧收发诊断计数（只增不减，32 位读写原子）
static volatile uint32_t g_tx_data_count = 0;
static volatile uint32_t g_rx_data_count = 0;
static volatile uint32_t g_deliver_count = 0;

// T-2：协议版本宣告诊断（BEACON 携带 1 字节协议版本）
static uint32_t g_proto_mismatch = 0;   // 版本不一致的信标计数
static uint8_t  g_peer_proto = 0;       // 最近一次收到的对端协议版本（0 = 未收到/老版本无此字段）

// ping 等待状态（mesh_ping 阻塞期间，mesh_task 收到匹配 ACK 时置位）
static volatile uint16_t g_ping_wait_seq = 0;   // 0 = 无等待
static volatile bool     g_ping_done = false;
static volatile int16_t  g_ping_rssi = 0;
static volatile uint32_t g_ping_start_ms = 0;

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

// 找源对应的槽；没有空槽时淘汰"最久未见"的源（原实现返回 nullptr → 整表停止判重）
static dedup_slot_t* dedup_slot(uint8_t src_id) {
    dedup_slot_t* free_slot = nullptr;
    dedup_slot_t* oldest    = nullptr;
    for (int i = 0; i < MESH_MAX_SRC; i++) {
        if (g_dedup[i].src_id == src_id) return &g_dedup[i];
        if (g_dedup[i].src_id == 0) { if (!free_slot) free_slot = &g_dedup[i]; continue; }
        if (!oldest || (int32_t)(g_dedup[i].last_seen_ms - oldest->last_seen_ms) < 0) oldest = &g_dedup[i];
    }
    if (free_slot) return free_slot;
    if (oldest) {           // 表满：淘汰最旧的源
        oldest->src_id = 0; // 置空，交给 dedup_check 重新初始化
        g_dedup_evict++;
        return oldest;
    }
    return nullptr;
}

// 去重检查：返回 true=重复（丢弃）
static bool dedup_check(uint8_t src_id, uint16_t seq) {
    dedup_slot_t* s = dedup_slot(src_id);
    if (!s) return false;   // 不可能到达：dedup_slot 已保证有槽

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
    // 序号历史环归零（0xFFFF = 空槽）；撞号计数清零
    for (int i = 0; i < MESH_SEQ_HISTORY; i++) g_my_seq_hist[i] = 0xFFFF;
    g_my_seq_idx  = 0;
    g_id_conflict = 0;
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

// 分配发送序号（跨核原子）
uint16_t mesh_next_seq() {
    portENTER_CRITICAL(&g_seq_mux);
    uint16_t s = g_seq++;
    // 写入历史环（保持唯一 seq 源不变，仅记录用于撞号判定）
    g_my_seq_hist[g_my_seq_idx] = s;
    g_my_seq_idx = (uint8_t)((g_my_seq_idx + 1) % MESH_SEQ_HISTORY);
    portEXIT_CRITICAL(&g_seq_mux);
    return s;
}

// seq 是否在本机最近发出的序号里（跨核查历史环）
static bool seq_is_mine(uint16_t s) {
    bool found = false;
    portENTER_CRITICAL(&g_seq_mux);
    for (uint8_t i = 0; i < MESH_SEQ_HISTORY; i++) {
        if (g_my_seq_hist[i] == s) { found = true; break; }
    }
    portEXIT_CRITICAL(&g_seq_mux);
    return found;
}

// 本机发起发送：决定 relay_id（单播查下一跳，未知回退广播）
bool mesh_send(uint8_t dst_id, uint8_t qos, bool ack_req,
               const uint8_t* payload, size_t len, uint8_t orig_sysid) {
    if (!payload || len > FRAME_MAX_PLAIN) return false;

    frame_t f = {};
    f.type       = FRAME_DATA;
    f.qos        = qos;
    f.ack_req    = ack_req;
    f.frag       = false;
    f.proto      = PROTO_MAVLINK;
    f.src_id     = g_my_node_id;
    f.dst_id     = dst_id;
    f.relay_id   = 0;
    f.seq        = mesh_next_seq();
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

    // 加密（启用时原地改 payload 为密文；seal_buf 覆盖到 frame_encode 拷贝完成）
    uint8_t seal_buf[FRAME_MAX_PAYLOAD];
    crypto_seal_frame(&f, seal_buf, sizeof(seal_buf));

    uint8_t buf[FRAME_MTU];
    size_t n = frame_encode(&f, buf, sizeof(buf));
    if (n == 0) return false;

    queue_item_t item = {};
    memcpy(item.data, buf, n);
    item.len = (uint16_t)n;
    item.qos = qos;
    item.ack_req = ack_req ? 1 : 0;
    bool ok = rtos_tx_enqueue(&item);
    if (ok) g_tx_data_count++;
    return ok;
}

// ping 探测：单播发 FRAME_PING，等 ACK 测 RTT
int32_t mesh_ping(uint8_t dst_id, uint32_t timeout_ms, int16_t* rssi_out) {
    frame_t f = {};
    f.type       = FRAME_PING;
    f.qos        = QOS1;
    f.ack_req    = true;
    f.frag       = false;
    f.proto      = PROTO_CUSTOM;
    f.src_id     = g_my_node_id;
    f.dst_id     = dst_id;
    f.relay_id   = 0;
    f.seq        = mesh_next_seq();
    f.ttl        = g_my_ttl;
    f.orig_sysid = 0;
    f.payload    = nullptr;
    f.payload_len = 0;

    // 单播路径（同 mesh_send 的 relay 逻辑）
    if (dst_id != 0) {
        uint8_t nh = route_next_hop(dst_id);
        if (nh == 0) {
            f.dst_id = 0;            // 路径未知回退广播
        } else if (nh != dst_id) {
            f.relay_id = nh;         // 经中继
        }
    }

    uint8_t buf[FRAME_MTU];
    size_t n = frame_encode(&f, buf, sizeof(buf));
    if (n == 0) return -1;

    queue_item_t item = {};
    memcpy(item.data, buf, n);
    item.len = (uint16_t)n;
    item.qos = QOS1;
    item.ack_req = 1;

    // 登记等待，再投队列（避免 ACK 先于登记到达）
    g_ping_wait_seq = f.seq;
    g_ping_done     = false;
    g_ping_rssi     = 0;
    g_ping_start_ms = millis();

    if (!rtos_tx_enqueue(&item)) { g_ping_wait_seq = 0; return -1; }

    // 轮询等 ACK（mesh_task 收到匹配 ACK 时置 g_ping_done）
    uint32_t deadline = millis() + timeout_ms;
    while (!g_ping_done && (int32_t)(millis() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!g_ping_done) { g_ping_wait_seq = 0; return -1; }   // 超时

    int32_t rtt = (int32_t)(millis() - g_ping_start_ms);
    g_ping_wait_seq = 0;
    if (rssi_out) *rssi_out = g_ping_rssi;
    return rtt;
}

// 发控制帧（NACK / CFG 等）：type 自选，payload 走加密
bool mesh_send_ctrl(uint8_t type, uint8_t dst_id, uint8_t qos, bool ack_req,
                    const uint8_t* payload, size_t len) {
    if (len > FRAME_MAX_PLAIN) return false;

    frame_t f = {};
    f.type       = type;
    f.qos        = qos;
    f.ack_req    = ack_req;
    f.frag       = false;
    f.proto      = PROTO_CUSTOM;
    f.src_id     = g_my_node_id;
    f.dst_id     = dst_id;
    f.relay_id   = 0;
    f.seq        = mesh_next_seq();
    f.ttl        = g_my_ttl;
    f.orig_sysid = 0;
    f.payload    = payload;
    f.payload_len = len;

    if (dst_id != 0) {
        uint8_t nh = route_next_hop(dst_id);
        if (nh == 0) {
            f.dst_id = 0;            // 路径未知回退广播
        } else if (nh != dst_id) {
            f.relay_id = nh;         // 经中继
        }
    }

    uint8_t seal_buf[FRAME_MAX_PAYLOAD];
    crypto_seal_frame(&f, seal_buf, sizeof(seal_buf));

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

bool mesh_send_beacon(uint8_t sender_role) {
    frame_t f = {};
    f.type       = FRAME_BEACON;
    f.qos        = QOS0;
    f.ack_req    = false;
    f.frag       = false;
    f.proto      = PROTO_CUSTOM;   // BEACON 无 MAVLink 载荷
    f.src_id     = g_my_node_id;
    f.dst_id     = 0;              // 广播
    f.relay_id   = 0;
    f.seq        = mesh_next_seq();
    f.ttl        = MESH_BEACON_TTL;   // 允许 1 跳转发（P1-3）
    f.orig_sysid = 0;
    // v3：BEACON payload = 2 字节，{协议版本, 发送者角色}（不加密）。
    // pl 必须活到 frame_encode 拷贝完成（payload 指针未被提前释放）。
    uint8_t pl[2] = { EM_PROTO_VERSION, sender_role };
    f.payload    = pl;
    f.payload_len = 2;

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
uint32_t mesh_dedup_evict_count() { return g_dedup_evict; }
uint32_t mesh_drop_auth_count() { return g_drop_auth; }
uint32_t mesh_rx_count()       { return g_rx; }
uint32_t mesh_last_rx_ms()     { return g_last_rx_ms; }
uint32_t mesh_tx_data_count()  { return g_tx_data_count; }
uint32_t mesh_rx_data_count()  { return g_rx_data_count; }
uint32_t mesh_deliver_count()  { return g_deliver_count; }
uint32_t mesh_id_conflict_count() { return g_id_conflict; }

// T-2：协议版本宣告诊断
uint32_t mesh_proto_mismatch_count(void) { return g_proto_mismatch; }
uint8_t  mesh_peer_proto_version(void)   { return g_peer_proto; }

uint8_t mesh_my_id(void) { return g_my_node_id; }

// ---- mesh_task：收帧 -> 解码 -> ACK/去重/TTL -> 投递/转发 ----
static void mesh_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    queue_item_t item;
    for (;;) {
        watchdog_feed();
        // 带超时收帧：超时也跑 frag_tick（缺件 NACK 检查，~100ms 粒度）
        if (xQueueReceive(q_rf_rx, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            frag_tick();
            continue;
        }

        frame_t f;
        if (!frame_decode(item.data, item.len, &f)) {
            g_drop_crc++;
            continue;
        }
        g_rx++;
        g_last_rx_ms = millis();
        if (f.type == FRAME_DATA) g_rx_data_count++;

        // 撞号检测：收到“自称本机 node_id”的帧，但 seq 不在本机最近 32 个已发序号里
        // → 极有可能是另一台节点用了同一个 node_id（本机自己中继回来的帧会在历史里，不计）
        if (f.src_id == g_my_node_id && !seq_is_mine(f.seq)) {
            g_id_conflict++;
        }

        // ACK 帧：通知 mac + 匹配 ping 等待
        if (f.type == FRAME_ACK) {
            mac_on_ack_rx(f.src_id, f.seq);
            if (g_ping_wait_seq != 0 && f.seq == g_ping_wait_seq) {
                g_ping_done = true;
                g_ping_rssi = item.rssi;
            }
            continue;
        }

        // 去重（重复帧不处理不转发）
        if (!mesh_ingress(&f)) continue;

        // BEACON：记录邻居链路质量 + 对齐 ADR 周期；不投递，但允许转发（P1-3）
        if (f.type == FRAME_BEACON) {
            route_learn(f.src_id, f.relay_id);
            route_update_rssi(f.src_id, item.rssi);
            // v3：第 2 字节是发送者角色；旧固件只有 1 字节 → 按 GCS 处理，退回旧行为
            uint8_t sender_role = (f.payload_len >= 2) ? f.payload[1] : ROLE_GCS;
            adr_on_beacon(f.src_id, f.relay_id, sender_role);   // 非 GCS 信标只算可见性，不对齐

            // T-2：协议版本宣告比对（只告警不拒绝）。
            // payload_len>=1 → 对端版本；==0 → 老固件/无版本字段，记 0。
            uint8_t peer_ver = (f.payload_len >= 1) ? f.payload[0] : 0;
            g_peer_proto = peer_ver;
            if (peer_ver != 0 && peer_ver != EM_PROTO_VERSION) {
                g_proto_mismatch++;
                static uint8_t warned_ver = 0;   // 每个对端版本只告警一次，避免每周期刷屏
                if (warned_ver != peer_ver) {
                    warned_ver = peer_ver;
                    Serial.printf("[mesh] 协议版本不一致：本机 v%u，对端 v%u（帧格式/封装不兼容，需全网同版本固件）\n",
                                  (unsigned)EM_PROTO_VERSION, (unsigned)peer_ver);
                }
            }
            // 不 continue：让信标继续走到下面的转发判定（dst=0、ttl>0 → 中继转发一次）
        } else if (f.type == FRAME_PING) {
            // PING：回 ACK，不投递不转发（探测帧到此为止）
            mac_send_ack(f.src_id, f.seq);
            continue;
        }

        // 数据帧且要 ACK：回 ACK
        if (f.type == FRAME_DATA && f.ack_req) {
            mac_send_ack(f.src_id, f.seq);
        }

        // 反向路径学习（广播帧：relay_id 即上一跳）
        if (f.dst_id == 0) route_learn(f.src_id, f.relay_id);

        // 投递本机（dst 是自己或广播）；加密帧先解密
        // P1-8：认证/解密失败只影响本机投递，不影响转发——转发的是密文，中继不需要密钥
        if ((f.type == FRAME_DATA || f.type == FRAME_NACK || f.type == FRAME_CFG) &&
            (f.dst_id == 0 || f.dst_id == g_my_node_id)) {
            frame_t rx = f;
            uint8_t plain[FRAME_MTU];
            bool deliver = true;
            if (f.enc) {
                size_t plen = crypto_open_frame(&f, plain, sizeof(plain));
                if (plen == 0) {
                    g_drop_auth++;      // 认证失败：本机不投递（但仍按 TTL 转发）
                    deliver = false;
                } else if (!crypto_check_replay(f.src_id, f.seq, f.enc_epoch)) {
                    // 重放/过旧：不投递（转发照旧）
                    deliver = false;
                } else {
                    rx.payload     = plain;
                    rx.payload_len = plen;
                    rx.enc         = false;
                }
            }
            if (deliver && g_rx_cb) {
                g_rx_cb(&rx, (float)item.rssi, (float)item.snr);
                g_deliver_count++;
            }
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
