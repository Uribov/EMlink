// ============================================================
// frag.cpp —— MAVLink 大包分片 / 重组 / 缺件计数
// 依赖 frame_codec.h（FRAME_MAX_PAYLOAD=180）；约定见架构 §6
// ============================================================
#include "frag.h"
#include <string.h>

#define FRAG_CONCURRENT 4
#define FRAG_MAX_PIECES 8

struct reasm_slot_t {
    bool     in_use;
    uint8_t  orig_sysid;
    uint8_t  frag_seq;
    uint8_t  frag_total;
    uint8_t  got;                       // 位图（低 8 位够 8 片）
    uint32_t start_ms;
    uint8_t  data[FRAG_MAX_PIECES * FRAME_MAX_PAYLOAD];
    size_t   piece_len[FRAG_MAX_PIECES];
};

static reasm_slot_t g_reasm[FRAG_CONCURRENT];
static uint8_t g_frag_seq = 0;
static uint32_t g_timeout = 0;

void frag_init() {
    memset(g_reasm, 0, sizeof(g_reasm));
    g_frag_seq = 0;
    g_timeout = 0;
}

size_t frag_split(const uint8_t* data, size_t len,
                  uint8_t out[][FRAME_MAX_PAYLOAD], size_t max_frags,
                  frag_hdr_t* hdr) {
    if (!data || !out) return 0;

    if (len <= FRAME_MAX_PAYLOAD) {
        if (max_frags >= 1) memcpy(out[0], data, len);
        if (hdr) { hdr->msg_id = 0; hdr->frag_index = 0; hdr->frag_total = 1; hdr->frag_seq = g_frag_seq; }
        return 1;
    }

    size_t total = (len + FRAME_MAX_PAYLOAD - 1) / FRAME_MAX_PAYLOAD;
    if (total > max_frags || total > FRAG_MAX_PIECES) return 0;

    if (hdr) {
        hdr->msg_id     = 0;
        hdr->frag_seq   = g_frag_seq++;   // 组序号自增
        hdr->frag_total = (uint8_t)total;
        // frag_index 由调用者每片递增
    }

    for (size_t i = 0; i < total; i++) {
        size_t off = i * FRAME_MAX_PAYLOAD;
        size_t n = (len - off < FRAME_MAX_PAYLOAD) ? (len - off) : FRAME_MAX_PAYLOAD;
        memcpy(out[i], data + off, n);
    }
    return total;
}

void frag_reassemble(uint8_t orig_sysid, const frag_hdr_t* hdr, const uint8_t* data, size_t len,
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
        if (g_reasm[i].in_use && g_reasm[i].orig_sysid == orig_sysid && g_reasm[i].frag_seq == hdr->frag_seq) { s = &g_reasm[i]; break; }
    }
    if (!s) {
        for (int i = 0; i < FRAG_CONCURRENT; i++) {
            if (!g_reasm[i].in_use) { s = &g_reasm[i]; break; }
        }
        if (!s) return;   // 槽满
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->orig_sysid = orig_sysid;
        s->frag_seq = hdr->frag_seq;
        s->frag_total = hdr->frag_total;
        s->start_ms = now;
    }

    // 存片
    if (hdr->frag_index < FRAG_MAX_PIECES && len <= FRAME_MAX_PAYLOAD) {
        size_t off = (size_t)hdr->frag_index * FRAME_MAX_PAYLOAD;
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

uint32_t frag_timeout_count() { return g_timeout; }
