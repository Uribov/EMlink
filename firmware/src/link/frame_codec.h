#pragma once
// ============================================================
// frame_codec —— Mesh 帧编解码（组帧/拆帧/CRC/序号）
// 帧格式按 ../../mesh_firmware_architecture.md §4.1（含 relay_id 扩展）
// ============================================================
#include <Arduino.h>

// ---- 帧常量 ----
#define FRAME_MTU          200    // 空中总长上限（含 LoRa 物理头余量）
#define FRAME_HDR_LEN      15     // 偏移 0~14（ver..frag_hdr，含 relay_id、enc_epoch）
#define FRAME_CRC_LEN      2      // 偏移 194~195
#define FRAME_MAX_PAYLOAD  179    // 偏移 15~193；MAVLink 分片阈值
#define FRAME_DEF_TTL      5      // 默认剩余跳数（最多 4 跳）

// 加密（crypto 模块）相关：tag 长度 + 加密帧明文上限
#define FRAME_TAG_LEN      4      // AES-CCM tag 4B（密文末尾）
#define FRAME_MAX_PLAIN    175    // 加密帧明文上限 = FRAME_MAX_PAYLOAD - FRAME_TAG_LEN

// 帧类型（type 低 4 位）
enum : uint8_t {
    FRAME_DATA   = 0,
    FRAME_ACK    = 1,
    FRAME_NACK   = 2,
    FRAME_BEACON = 3,
    FRAME_PING   = 4,
    FRAME_ROUTE  = 5,   // 预留
    FRAME_CFG    = 6,   // 远程参数同步（GCS 广播 key=value）
};

// 协议类型（flags[3:2]，payload 里装的是什么协议）
enum : uint8_t {
    PROTO_MAVLINK = 0,
    PROTO_CRSF    = 1,
    PROTO_MSP     = 2,
    PROTO_CUSTOM  = 3,
};

// QoS 档位
enum : uint8_t { QOS0 = 0, QOS1 = 1, QOS2 = 2 };

// flags 位（偏移 1）
#define FLAG_ACK_REQ    (1u << 5)
#define FLAG_FRAG       (1u << 4)
#define FLAG_ENC        (1u << 0)               // 加密标志（crypto 模块，payload 为密文）
#define FLAG_PROTO_MASK (0x0Cu)                 // proto 占 [3:2]
#define FLAG_PROTO(x)   (((x) << 2) & FLAG_PROTO_MASK)

// ---- 帧结构（内存表示，非线上布局）----
// 线上布局（偏移）：
//   0 ver:type | 1 flags | 2 src_id | 3 dst_id | 4-5 seq(LE) | 6 ttl
//   7 relay_id | 8 orig_sysid | 9 net_id | 10 enc_epoch | 11-14 frag_hdr | 15.. payload | CRC
struct frame_t {
    uint8_t  type;         // FRAME_*
    uint8_t  qos;          // QOS0/1/2
    bool     ack_req;      // 需要 ACK
    bool     frag;         // 是否分片帧
    bool     enc;          // 是否加密（flags bit0；payload 为密文）
    uint8_t  proto;        // PROTO_*（payload 协议）
    uint8_t  src_id;       // 源节点地址 1~254
    uint8_t  dst_id;       // 目的节点地址；0=广播
    uint8_t  relay_id;     // 广播帧=上一跳转发者；单播帧=下一跳；源发=0
    uint8_t  net_id;       // 网络 ID（多网共存，0=默认网）
    uint8_t  enc_epoch;    // 开机世代号（AES-CCM nonce 用，防跨重启 nonce 复用；未加密帧=0）
    uint16_t seq;          // 源节点序号（回绕）
    uint8_t  ttl;          // 剩余跳数
    uint8_t  orig_sysid;   // 源 MAVLink sysid（供 addr_map 学习/重组键）
    uint8_t  frag_hdr[4];  // 分片头（非分片帧填 0）
    const uint8_t* payload;
    size_t   payload_len;
};

// ---- 接口 ----
// 内存结构 -> 线上字节（buf 容量 >= FRAME_MTU）；返回总长，失败返回 0
size_t frame_encode(const frame_t* f, uint8_t* buf, size_t cap);

// 线上字节 -> 内存结构；CRC 通过返回 true（payload 指针指向 buf 内部）
bool frame_decode(const uint8_t* buf, size_t len, frame_t* f);

// CRC16-CCITT，覆盖 header+payload
uint16_t frame_crc16(const uint8_t* data, size_t len);
