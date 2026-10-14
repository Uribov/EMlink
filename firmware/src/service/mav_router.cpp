// ============================================================
// mav_router.cpp —— MAVLink 2 解析、按 target_sysid 分发、心跳优先
// 只支持 MAVLink 2（STX=0xFD）。依赖 mavlink 库（lib/mavlink2）、frag、addr_map。
// ============================================================
#include "mav_router.h"
#include "../rtos.h"
#include "../link/frame_codec.h"
#include "frag.h"
#include "addr_map.h"
#include "crypto.h"
#include "../net/mesh.h"
#include "../drivers/uart_fc.h"
#include "../drivers/usb_cdc.h"
#include "../service/nvs_cfg.h"
#include "../service/watchdog.h"
#include <common/mavlink.h>      // lib/mavlink2

// 来源
enum { SRC_UART = 0, SRC_USB = 1, SRC_MESH = 2 };

static uint8_t g_local_sysid = 0;
static uint8_t g_role = ROLE_UNSET;
static uint8_t g_node_id = 1;
static uint8_t g_ttl = FRAME_DEF_TTL;

// 接收状态机（uart / usb 各独立一套；mesh 来源是完整消息，不走状态机）
struct rx_state_t {
    uint8_t buf[300];
    size_t  len;
    size_t  expected;
};
static rx_state_t g_rx_uart;
static rx_state_t g_rx_usb;

// 心跳
static volatile uint32_t g_last_hb_ms = 0;

// 最近一次分片发送缓存（NACK 补发用）。跨核访问（core1 发、core0 补），加锁。
static SemaphoreHandle_t g_tx_cache_lock = nullptr;
static uint8_t  g_tx_cache[4 * FRAME_MAX_PLAIN];   // 最多 4 片
static size_t   g_tx_cache_len = 0;                // 原始数据总长
static uint8_t  g_tx_cache_sysid = 0;
static uint8_t  g_tx_cache_frag_seq = 0;           // 该组分片号
static uint8_t  g_tx_cache_total = 0;              // 总片数
static uint8_t  g_tx_cache_qos = QOS0;

// 补发缓冲（静态，避免 mesh_task 栈上放大数组）；仅 on_nack 用，单线程访问
static uint8_t  g_resend[4 * FRAME_MAX_PLAIN];

static TaskHandle_t g_mavlink_task = nullptr;

// ---- CRC 校验（CRC-16/MCRF4XX，含 crc_extra）----
static bool mav_crc_ok(const uint8_t* buf, size_t len) {
    if (len < MAVLINK2_HEADER_LEN + MAVLINK2_CRC_LEN) return false;
    uint8_t payload_len = buf[1];
    if (len != MAVLINK2_HEADER_LEN + payload_len + MAVLINK2_CRC_LEN) return false;

    uint32_t msgid = (uint32_t)buf[7] | ((uint32_t)buf[8] << 8) | ((uint32_t)buf[9] << 16);
    uint8_t crc_extra = 0;
    const mavlink_msg_entry_t* e = mavlink_get_msg_entry(msgid);
    if (e) crc_extra = e->crc_extra;

    uint16_t crc = crc_calculate(buf + 1, MAVLINK2_HEADER_LEN - 1 + payload_len);
    crc_accumulate(crc_extra, &crc);

    uint16_t crc_recv = (uint16_t)(buf[len - 2] | (buf[len - 1] << 8));
    return crc == crc_recv;
}

void mav_router_init(uint8_t local_sysid) {
    g_local_sysid = local_sysid;
    memset(&g_rx_uart, 0, sizeof(g_rx_uart));
    memset(&g_rx_usb, 0, sizeof(g_rx_usb));
    if (!g_tx_cache_lock) g_tx_cache_lock = xSemaphoreCreateMutex();
    cfg_t c;
    if (nvs_cfg_load(&c)) {
        g_role = c.role;
        if (c.node_id != 0) g_node_id = c.node_id;
        if (c.ttl != 0)     g_ttl = c.ttl;
    }
}

bool mav_router_heartbeat_ok() {
    return (millis() - g_last_hb_ms) < 5000;   // 5s 内有心跳视为在线
}

// 收到 NACK：从缓存补发缺失分片
void mav_router_on_nack(const uint8_t* payload, size_t len) {
    if (!payload || len < 3) return;
    if (!g_tx_cache_lock) return;

    uint8_t sysid = payload[0];
    uint8_t fseq  = payload[1];
    uint8_t missing = payload[2];

    // 匹配缓存：sysid + frag_seq 都对上才补
    xSemaphoreTake(g_tx_cache_lock, portMAX_DELAY);
    if (g_tx_cache_len == 0 || sysid != g_tx_cache_sysid || fseq != g_tx_cache_frag_seq) {
        xSemaphoreGive(g_tx_cache_lock);
        return;   // 不是我们发的组 / 已过期
    }

    // 复制缓存到静态补发缓冲（锁内短，锁外慢发）
    size_t  dlen = g_tx_cache_len;
    uint8_t total = g_tx_cache_total;
    uint8_t qos   = g_tx_cache_qos;
    memcpy(g_resend, g_tx_cache, dlen);
    xSemaphoreGive(g_tx_cache_lock);

    // 逐片补发缺失片
    for (uint8_t i = 0; i < total; i++) {
        if (!(missing & (1u << i))) continue;   // 不缺这片

        size_t off  = (size_t)i * FRAME_MAX_PLAIN;
        size_t plen = (i == total - 1) ? (dlen - off) : FRAME_MAX_PLAIN;
        if (plen > FRAME_MAX_PLAIN) continue;

        frag_hdr_t h = {};
        h.msg_id = 0; h.frag_index = i; h.frag_total = total; h.frag_seq = fseq;

        frame_t f = {};
        f.type = FRAME_DATA; f.qos = qos; f.ack_req = false; f.frag = true;
        f.proto = PROTO_MAVLINK; f.src_id = g_node_id; f.dst_id = 0;
        f.seq = mesh_next_seq(); f.ttl = g_ttl; f.orig_sysid = sysid;
        memcpy(f.frag_hdr, &h, 4);
        f.payload = g_resend + off; f.payload_len = plen;

        uint8_t seal_buf[FRAME_MAX_PAYLOAD];
        crypto_seal_frame(&f, seal_buf, sizeof(seal_buf));

        uint8_t buf[FRAME_MTU];
        size_t bn = frame_encode(&f, buf, sizeof(buf));
        if (bn) {
            queue_item_t item = {};
            memcpy(item.data, buf, bn);
            item.len = (uint16_t)bn; item.qos = qos; item.ack_req = 0;
            rtos_tx_enqueue(&item);
        }
    }
}

// ---- 路由：决定送本地串口还是转 LoRa ----
static void route_msg(const uint8_t* msg, size_t len, uint8_t source) {
    if (len < MAVLINK2_HEADER_LEN + MAVLINK2_CRC_LEN) return;

    uint8_t  sysid       = msg[5];
    uint8_t  payload_len = msg[1];
    uint32_t msgid       = (uint32_t)msg[7] | ((uint32_t)msg[8] << 8) | ((uint32_t)msg[9] << 16);
    const uint8_t* payload = &msg[MAVLINK2_HEADER_LEN];

    // 心跳：更新状态
    if (msgid == MAV_MSGID_HEARTBEAT) {
        g_last_hb_ms = millis();
    }

    // 提取 target_sysid（命令/请求类消息才有，按 mavlink 库消息条目）
    uint8_t target_sysid = 0;
    bool    has_target = false;
    const mavlink_msg_entry_t* e = mavlink_get_msg_entry(msgid);
    if (e && e->target_system_ofs < payload_len) {
        target_sysid = payload[e->target_system_ofs];
        has_target = (e->flags & MAV_MSG_ENTRY_FLAG_HAVE_TARGET_SYSTEM) != 0;
    }

    // 决定是否转 LoRa（发到空中）
    bool to_lora = false;
    switch (g_role) {
        case ROLE_AIR:
            if (source == SRC_UART) to_lora = true;          // 飞控上行 -> LoRa
            else if (source == SRC_MESH) {
                // GCS 下行 -> 只看是否发给自己飞控
                if (!has_target || target_sysid == 0 || target_sysid == g_local_sysid) {
                    uart_fc_write(msg, len);
                }
            }
            break;
        case ROLE_GCS:
            if (source == SRC_USB) to_lora = true;           // GCS 下行 -> LoRa
            else if (source == SRC_MESH) {
                usb_mav_write(msg, len);                     // AIR 上行 -> USB
            }
            break;
        case ROLE_RELAY:
            break;   // 纯转发在 mesh 层
        default:
            break;
    }

    if (to_lora) {
        uint8_t qos = (msgid == MAV_MSGID_HEARTBEAT) ? QOS2 : QOS0;   // 心跳最高优先
        // 大消息分片（明文上限 175，加密后每片密文 179 正好塞满 payload 区）
        if (len > FRAME_MAX_PLAIN) {
            frag_hdr_t hdr = {};
            uint8_t pieces[4][FRAME_MAX_PLAIN];
            size_t n = frag_split(msg, len, pieces, 4, &hdr);

            // 缓存原始数据（NACK 补发用）
            if (g_tx_cache_lock && len <= sizeof(g_tx_cache)) {
                xSemaphoreTake(g_tx_cache_lock, portMAX_DELAY);
                memcpy(g_tx_cache, msg, len);
                g_tx_cache_len      = len;
                g_tx_cache_sysid    = sysid;
                g_tx_cache_frag_seq = hdr.frag_seq;
                g_tx_cache_total    = (uint8_t)n;
                g_tx_cache_qos      = qos;
                xSemaphoreGive(g_tx_cache_lock);
            }

            for (size_t i = 0; i < n; i++) {
                size_t off  = i * FRAME_MAX_PLAIN;
                size_t plen = (i == n - 1) ? (len - off) : FRAME_MAX_PLAIN;
                frag_hdr_t h = hdr;
                h.frag_index = (uint8_t)i;

                frame_t f = {};
                f.type = FRAME_DATA; f.qos = qos; f.ack_req = false; f.frag = true;
                f.proto = PROTO_MAVLINK; f.src_id = g_node_id; f.dst_id = 0;
                f.seq = mesh_next_seq(); f.ttl = g_ttl; f.orig_sysid = sysid;
                memcpy(f.frag_hdr, &h, 4);
                f.payload = pieces[i]; f.payload_len = plen;

                // 加密（启用时原地改 payload 为密文）
                uint8_t seal_buf[FRAME_MAX_PAYLOAD];
                crypto_seal_frame(&f, seal_buf, sizeof(seal_buf));

                uint8_t buf[FRAME_MTU];
                size_t bn = frame_encode(&f, buf, sizeof(buf));
                if (bn) {
                    queue_item_t item = {};
                    memcpy(item.data, buf, bn);
                    item.len = (uint16_t)bn; item.qos = qos; item.ack_req = 0;
                    rtos_tx_enqueue(&item);
                }
            }
        } else {
            mesh_send(0, qos, false, msg, len, sysid);   // 广播
        }
    }
}

void mav_router_handle_msg(const uint8_t* msg, size_t len) {
    // 公开接口：默认按 mesh 来源处理（主要供 mesh_rx_cb 透传调用）
    route_msg(msg, len, SRC_MESH);
}

// 通用字节流切帧：喂入一段字节，切出完整 MAVLink 帧后回调 route_msg
static void feed_bytes(rx_state_t* st, const uint8_t* data, size_t len, uint8_t source) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (st->len == 0) {
            if (c == MAVLINK2_STX) { st->buf[0] = c; st->len = 1; }
        } else if (st->len == 1) {
            st->buf[1] = c;
            st->expected = MAVLINK2_HEADER_LEN + c + MAVLINK2_CRC_LEN;
            if (st->expected > sizeof(st->buf)) { st->len = 0; continue; }
            st->len = 2;
        } else {
            st->buf[st->len++] = c;
            if (st->len == st->expected) {
                if (mav_crc_ok(st->buf, st->len)) {
                    route_msg(st->buf, st->len, source);
                }
                st->len = 0;
            }
        }
    }
}

void mav_router_feed(const uint8_t* data, size_t len) {
    // 来自 mesh 的完整 MAVLink 消息（mesh 每帧 payload 就是一条完整消息，
    // 分片重组后也是完整消息），无需字节流切帧，直接路由。
    if (len >= MAVLINK2_HEADER_LEN + MAVLINK2_CRC_LEN && mav_crc_ok(data, len)) {
        route_msg(data, len, SRC_MESH);
    }
}

// 内部：来自 UART / USB 的字节流（mavlink_task 用），各用独立状态机
static void feed_uart(const uint8_t* data, size_t len) {
    feed_bytes(&g_rx_uart, data, len, SRC_UART);
}

static void feed_usb(const uint8_t* data, size_t len) {
    feed_bytes(&g_rx_usb, data, len, SRC_USB);
}

// ---- mavlink_task：从 q_uart_rx / q_usb_rx 收 MAVLink 字节流 ----
static void mavlink_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    queue_item_t item;
    for (;;) {
        watchdog_feed();
        if (xQueueReceive(q_uart_rx, &item, 0) == pdTRUE) {
            feed_uart(item.data, item.len);
        }
        if (xQueueReceive(q_usb_rx, &item, 0) == pdTRUE) {
            feed_usb(item.data, item.len);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void mav_router_start(void) {
    xTaskCreatePinnedToCore(mavlink_task_func, "mavlink", MAVLINK_TASK_STACK,
                            nullptr, MAVLINK_TASK_PRIO, &g_mavlink_task, MAVLINK_TASK_CORE);
}
