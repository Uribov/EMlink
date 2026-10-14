// ============================================================
// frame_codec.cpp —— Mesh 帧编解码（组帧/拆帧/CRC16）
// 纯逻辑，无硬件依赖。
// ============================================================
#include "frame_codec.h"

// CRC16-CCITT（poly 0x1021，init 0xFFFF）
uint16_t frame_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc = (crc << 1);
        }
    }
    return crc;
}

// 内存结构 -> 线上字节；返回总长，失败返回 0
size_t frame_encode(const frame_t* f, uint8_t* buf, size_t cap) {
    if (!f || !buf) return 0;
    if (f->payload_len > FRAME_MAX_PAYLOAD) return 0;
    if (cap < FRAME_HDR_LEN + f->payload_len + FRAME_CRC_LEN) return 0;

    const size_t total = FRAME_HDR_LEN + f->payload_len + FRAME_CRC_LEN;

    buf[0] = (1 << 4) | (f->type & 0x0F);              // ver=1 + type
    buf[1] = ((f->qos & 0x03) << 6)
           | (f->ack_req ? FLAG_ACK_REQ : 0)
           | (f->frag    ? FLAG_FRAG   : 0)
           | (f->enc     ? FLAG_ENC    : 0)
           | FLAG_PROTO(f->proto);
    buf[2] = f->src_id;
    buf[3] = f->dst_id;
    buf[4] = (uint8_t)(f->seq & 0xFF);                 // seq 小端
    buf[5] = (uint8_t)((f->seq >> 8) & 0xFF);
    buf[6] = f->ttl;
    buf[7] = f->relay_id;
    buf[8] = f->orig_sysid;
    buf[9] = f->net_id;
    buf[10] = f->enc_epoch;
    memcpy(&buf[11], f->frag_hdr, 4);
    if (f->payload_len && f->payload)
        memcpy(&buf[15], f->payload, f->payload_len);

    // CRC 覆盖 header(15) + payload
    uint16_t crc = frame_crc16(buf, FRAME_HDR_LEN + f->payload_len);
    buf[total - 2] = (uint8_t)(crc & 0xFF);
    buf[total - 1] = (uint8_t)((crc >> 8) & 0xFF);

    return total;
}

// 线上字节 -> 内存结构；CRC 通过返回 true（payload 指向 buf 内部）
bool frame_decode(const uint8_t* buf, size_t len, frame_t* f) {
    if (!buf || !f) return false;
    if (len < FRAME_HDR_LEN + FRAME_CRC_LEN) return false;
    if ((buf[0] >> 4) != 1) return false;              // 版本必须为 1

    const size_t payload_len = len - FRAME_HDR_LEN - FRAME_CRC_LEN;
    if (payload_len > FRAME_MAX_PAYLOAD) return false;

    // CRC 校验（header + payload）
    uint16_t crc_calc = frame_crc16(buf, FRAME_HDR_LEN + payload_len);
    uint16_t crc_recv = (uint16_t)(buf[len - 2] | (buf[len - 1] << 8));
    if (crc_calc != crc_recv) return false;

    f->type       = buf[0] & 0x0F;
    f->qos        = (buf[1] >> 6) & 0x03;
    f->ack_req    = (buf[1] & FLAG_ACK_REQ) != 0;
    f->frag       = (buf[1] & FLAG_FRAG) != 0;
    f->enc        = (buf[1] & FLAG_ENC) != 0;
    f->proto      = (buf[1] & FLAG_PROTO_MASK) >> 2;
    f->src_id     = buf[2];
    f->dst_id     = buf[3];
    f->seq        = (uint16_t)(buf[4] | (buf[5] << 8));
    f->ttl        = buf[6];
    f->relay_id   = buf[7];
    f->orig_sysid = buf[8];
    f->net_id     = buf[9];
    f->enc_epoch  = buf[10];
    memcpy(f->frag_hdr, &buf[11], 4);
    f->payload     = &buf[15];
    f->payload_len = payload_len;

    return true;
}
