// ============================================================
// frag.cpp —— MAVLink 大包分片 / 重组 / 缺件计数
// 依赖 frame_codec.h（FRAME_MAX_PLAIN=175）；约定见架构 §6
// ============================================================
#include "frag.h"
#include "../net/mesh.h"
#include <string.h>

#define FRAG_CONCURRENT 4
#define FRAG_MAX_PIECES 8

struct reasm_slot_t {
    bool     in_use;
    uint8_t  src_id;                     // 发片源节点（NACK 回给它）
    uint8_t  orig_sysid;
    uint8_t  frag_seq;
    uint8_t  frag_total;
    uint8_t  got;                       // 位图（低 8 位够 8 片）
    bool     nack_sent;                 // 是否已发过 NACK（防风暴，只催一次）
    uint32_t start_ms;
    uint8_t  data[FRAG_MAX_PIECES * FRAME_MAX_PLAIN];
    size_t   piece_len[FRAG_MAX_PIECES];
};

static reasm_slot_t g_reasm[FRAG_CONCURRENT];
static uint8_t g_frag_seq = 0;
static uint32_t g_timeout = 0;
static uint32_t g_slot_evict = 0;   // 并发重组槽耗尽、被迫淘汰旧槽的次数

void frag_init() {
    memset(g_reasm, 0, sizeof(g_reasm));
    g_frag_seq = 0;
    g_timeout = 0;
    g_slot_evict = 0;
}

size_t frag_split(const uint8_t* data, size_t len,
                  uint8_t out[][FRAME_MAX_PLAIN], size_t max_frags,
                  frag_hdr_t* hdr) {
    if (!data || !out) return 0;

    if (len <= FRAME_MAX_PLAIN) {
        if (max_frags >= 1) memcpy(out[0], data, len);
        if (hdr) { hdr->msg_id = 0; hdr->frag_index = 0; hdr->frag_total = 1; hdr->frag_seq = g_frag_seq; }
        return 1;
    }

    size_t total = (len + FRAME_MAX_PLAIN - 1) / FRAME_MAX_PLAIN;
    if (total > max_frags || total > FRAG_MAX_PIECES) return 0;

    if (hdr) {
        hdr->msg_id     = 0;
        hdr->frag_seq   = g_frag_seq++;   // 组序号自增
        hdr->frag_total = (uint8_t)total;
        // frag_index 由调用者每片递增
    }

    for (size_t i = 0; i < total; i++) {
        size_t off = i * FRAME_MAX_PLAIN;
        size_t n = (len - off < FRAME_MAX_PLAIN) ? (len - off) : FRAME_MAX_PLAIN;
        memcpy(out[i], data + off, n);
    }
    return total;
}

void frag_reassemble(uint8_t src_id, uint8_t orig_sysid, const frag_hdr_t* hdr, const uint8_t* data, size_t len,
                     void (*done)(const uint8_t* full, size_t len)) {
    if (!hdr || !data || !done) return;
    uint32_t now = millis();

    // 懒清理超时槽
    for (int i = 0; i < FRAG_CONCURRENT; i++) {
        if (g_reasm[i].in_use && (now - g_reasm[i].start_ms) > FRAG_REASM_TIMEOUT_MS) {
            g_reasm[i].in_use = false;
            g_timeout++;
        }
    }

    reasm_slot_t* s = nullptr;
    for (int i = 0; i < FRAG_CONCURRENT; i++) {
        if (g_reasm[i].in_use && g_reasm[i].src_id == src_id &&
            g_reasm[i].orig_sysid == orig_sysid && g_reasm[i].frag_seq == hdr->frag_seq) { s = &g_reasm[i]; break; }
    }
    if (!s) {
        for (int i = 0; i < FRAG_CONCURRENT; i++) {
            if (!g_reasm[i].in_use) { s = &g_reasm[i]; break; }
        }
        if (!s) {
            // 槽满：淘汰开始最早的那个（最可能已无望拼齐），并计数——原实现直接 return，静默丢片
            for (int i = 0; i < FRAG_CONCURRENT; i++) {
                if (!s || (int32_t)(g_reasm[i].start_ms - s->start_ms) < 0) s = &g_reasm[i];
            }
            g_slot_evict++;
        }
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->src_id = src_id;
        s->orig_sysid = orig_sysid;
        s->frag_seq = hdr->frag_seq;
        s->frag_total = hdr->frag_total;
        s->start_ms = now;
    }

    // 存片
    if (hdr->frag_index < FRAG_MAX_PIECES && len <= FRAME_MAX_PLAIN) {
        size_t off = (size_t)hdr->frag_index * FRAME_MAX_PLAIN;
        memcpy(s->data + off, data, len);
        s->piece_len[hdr->frag_index] = len;
        s->got |= (1u << hdr->frag_index);
    }

    // 拼齐判断
    uint8_t full_mask = (s->frag_total >= 8) ? 0xFF : (uint8_t)((1u << s->frag_total) - 1);
    if ((s->got & full_mask) == full_mask) {
        size_t full_len = 0;
        for (uint8_t i = 0; i < s->frag_total; i++) full_len += s->piece_len[i];
        done(s->data, full_len);
        s->in_use = false;
    }
}

// 周期检查缺件：超 NACK 等待期还没拼齐 → 发 NACK 催源节点补缺片
void frag_tick() {
    uint32_t now = millis();
    for (int i = 0; i < FRAG_CONCURRENT; i++) {
        reasm_slot_t* s = &g_reasm[i];
        if (!s->in_use) continue;
        if (s->nack_sent) continue;
        if ((now - s->start_ms) < FRAG_NACK_WAIT_MS) continue;

        uint8_t full_mask = (s->frag_total >= 8) ? 0xFF : (uint8_t)((1u << s->frag_total) - 1);
        uint8_t missing = (~s->got) & full_mask;
        if (missing == 0) continue;   // 已拼齐（理论上走不到这，保险）

        // NACK payload：[orig_sysid, frag_seq, 缺失位图]
        uint8_t nack[3] = { s->orig_sysid, s->frag_seq, missing };
        mesh_send_ctrl(FRAME_NACK, s->src_id, QOS1, false, nack, 3);
        s->nack_sent = true;          // 只催一次，防 NACK 风暴
    }
}

uint32_t frag_timeout_count() { return g_timeout; }
uint32_t frag_slot_evict_count() { return g_slot_evict; }
