// ============================================================
// crypto.cpp —— 帧载荷加密（AES-128-CCM，mbedTLS）
// 依赖 mbedtls/ccm.h（Arduino-ESP32 框架自带，MBEDTLS_CCM_C 默认启用）。
// 密钥存 NVS（经 nvs_cfg），运行时缓存 g_key；全 0 = 关闭。
// ============================================================
#include "crypto.h"
#include "nvs_cfg.h"
#include <mbedtls/ccm.h>
#include <string.h>

static uint8_t g_key[CRYPTO_KEY_LEN];
static volatile bool g_enabled = false;
static uint8_t g_epoch = 0;                                     // 开机世代号（跨重启 nonce 不复用）
static portMUX_TYPE g_key_mux = portMUX_INITIALIZER_UNLOCKED;   // 跨核保护 g_key

static bool key_is_zero(void) {
    for (int i = 0; i < CRYPTO_KEY_LEN; i++)
        if (g_key[i] != 0) return false;
    return true;
}

void crypto_init() {
    portENTER_CRITICAL(&g_key_mux);
    memset(g_key, 0, sizeof(g_key));
    portEXIT_CRITICAL(&g_key_mux);
    cfg_t c;
    nvs_cfg_load(&c);
    portENTER_CRITICAL(&g_key_mux);
    memcpy(g_key, c.crypto_key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);
    g_enabled = !key_is_zero();

    // 每次开机 +1，保证跨重启不复用（即使加密关闭也要执行，保持世代号单调）
    g_epoch = nvs_cfg_next_enc_epoch();
}

bool crypto_enabled() { return g_enabled; }

uint8_t crypto_enc_epoch(void) { return g_epoch; }

// nonce[13] 布局：[0..8]=0（9B 预留）| [9]=enc_epoch | [10]=src_id | [11..12]=seq 小端
// (enc_epoch, src_id, seq) 三元组标识一帧：跨重启不复用（P1-4）
static void build_nonce(uint8_t nonce[CRYPTO_NONCE_LEN], uint8_t epoch, uint8_t src_id, uint16_t seq) {
    memset(nonce, 0, CRYPTO_NONCE_LEN);
    nonce[9]  = epoch;
    nonce[10] = src_id;
    nonce[11] = (uint8_t)(seq & 0xFF);
    nonce[12] = (uint8_t)((seq >> 8) & 0xFF);
}

// AAD = 逐跳不变的帧头字段（7B）：src_id, dst_id, type, enc_epoch, orig_sysid, seq(2B)
// 排除 ttl / relay_id：中继每跳都会改（ttl--、relay_id=本机），放进去会让正常转发在收端认证失败
static size_t build_aad(const frame_t* f, uint8_t aad[7]) {
    aad[0] = f->src_id;
    aad[1] = f->dst_id;
    aad[2] = f->type;
    aad[3] = f->enc_epoch;
    aad[4] = f->orig_sysid;
    aad[5] = (uint8_t)(f->seq & 0xFF);
    aad[6] = (uint8_t)((f->seq >> 8) & 0xFF);
    return 7;
}

bool crypto_seal_frame(frame_t* f, uint8_t* scratch, size_t scratch_cap) {
    if (!f || !g_enabled || !scratch) return false;
    if (f->payload_len == 0) return false;                 // 空 payload 不加密
    size_t total = f->payload_len + FRAME_TAG_LEN;
    if (total > FRAME_MAX_PAYLOAD || scratch_cap < total) return false;

    uint8_t nonce[CRYPTO_NONCE_LEN];
    // 先盖世代号再改 payload：收端靠帧头 enc_epoch 重建同一 nonce，漏掉会导致对端全部解密失败
    f->enc_epoch = g_epoch;
    build_nonce(nonce, g_epoch, f->src_id, f->seq);
    uint8_t aad[7];
    size_t aad_len = build_aad(f, aad);

    // 加锁拷 key 到局部（setkey 是耗时操作，锁只护 16B 拷贝，不护整个加解密）
    uint8_t key[CRYPTO_KEY_LEN];
    portENTER_CRITICAL(&g_key_mux);
    memcpy(key, g_key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int r = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (r == 0) {
        // 输出布局：密文（与明文等长）+ tag 4B；AAD 绑逐跳不变的帧头字段，防无密钥篡改
        r = mbedtls_ccm_encrypt_and_tag(&ctx, f->payload_len, nonce, CRYPTO_NONCE_LEN,
                                        aad, aad_len, f->payload, scratch,
                                        scratch + f->payload_len, FRAME_TAG_LEN);
    }
    mbedtls_ccm_free(&ctx);
    if (r != 0) return false;

    f->payload     = scratch;
    f->payload_len = total;
    f->enc         = true;
    return true;
}

size_t crypto_open_frame(const frame_t* f, uint8_t* out, size_t out_cap) {
    if (!f || !g_enabled || !f->enc || !out) return 0;
    if (f->payload_len < FRAME_TAG_LEN) return 0;
    size_t clen = f->payload_len - FRAME_TAG_LEN;         // 密文长 = 明文长
    if (out_cap < clen) return 0;

    uint8_t nonce[CRYPTO_NONCE_LEN];
    build_nonce(nonce, f->enc_epoch, f->src_id, f->seq);
    uint8_t aad[7];
    size_t aad_len = build_aad(f, aad);

    uint8_t key[CRYPTO_KEY_LEN];
    portENTER_CRITICAL(&g_key_mux);
    memcpy(key, g_key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int r = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (r == 0) {
        r = mbedtls_ccm_auth_decrypt(&ctx, clen, nonce, CRYPTO_NONCE_LEN,
                                     aad, aad_len, f->payload, out,
                                     f->payload + clen, FRAME_TAG_LEN);
    }
    mbedtls_ccm_free(&ctx);
    return (r == 0) ? clen : 0;
}

// ---- 重放窗（密文层第二道闸）----
// 去重表只管 30s/256 窗口，重放窗是密文层的第二道闸：窗口外的密文重放在此被拦下。
// 位图 64 位（bits[0] 低 32 + bits[1] 高 32），bit 0 = last_seq（最新），bit i = (last_seq - i)。
// epoch = 帧头 enc_epoch：它被 AAD 绑定（无密钥者改不动，改了会认证失败），
// 所以是"对方是否重启过"的可信信号——同源 epoch 变了 ⇒ 对方重启 ⇒ 重置该源窗口。
// 残余风险：持有旧世代真实密文的人可注入一帧陈旧但合法的帧，触发一次窗口重置。
#define CRYPTO_REPLAY_SRC_NUM  8
typedef struct { uint8_t src_id; uint16_t last_seq; uint32_t bits[2]; bool in_use; uint8_t epoch; } replay_slot_t;
static replay_slot_t g_replay[CRYPTO_REPLAY_SRC_NUM];
static uint32_t g_replay_drop = 0;
static uint32_t g_replay_resync = 0;
static portMUX_TYPE g_replay_mux = portMUX_INITIALIZER_UNLOCKED;

// 位图整体左移 n 位：bit i -> bit i+n，超界丢弃；n>=64 全清
static void replay_shift_left(replay_slot_t* s, int n) {
    if (n <= 0) return;
    if (n >= 64) { s->bits[0] = 0; s->bits[1] = 0; return; }
    if (n < 32) {
        uint32_t carry = s->bits[0] >> (32 - n);
        s->bits[0] <<= n;
        s->bits[1] = (s->bits[1] << n) | carry;
    } else {
        s->bits[1] = s->bits[0] << (n - 32);
        s->bits[0] = 0;
    }
}

// 找 src_id 的槽；没有 → 取空槽；无空槽 → 淘汰 last_seq 最旧者（按全新源处理）
static replay_slot_t* replay_slot(uint8_t src_id, uint16_t seq) {
    replay_slot_t* free_slot = nullptr;
    replay_slot_t* oldest    = nullptr;
    for (int i = 0; i < CRYPTO_REPLAY_SRC_NUM; i++) {
        if (g_replay[i].in_use && g_replay[i].src_id == src_id) return &g_replay[i];
        if (!g_replay[i].in_use) { if (!free_slot) free_slot = &g_replay[i]; continue; }
        if (!oldest || (int16_t)(seq - g_replay[i].last_seq) < 0) oldest = &g_replay[i];
    }
    if (free_slot) return free_slot;
    if (oldest) { oldest->src_id = 0; oldest->in_use = false; return oldest; }
    return nullptr;
}

bool crypto_check_replay(uint8_t src_id, uint16_t seq, uint8_t epoch) {
    portENTER_CRITICAL(&g_replay_mux);
    replay_slot_t* s = replay_slot(src_id, seq);
    if (!s) { portEXIT_CRITICAL(&g_replay_mux); return false; }

    if (!s->in_use || s->epoch != epoch) {
        // 全新源，或对方重启过（epoch 变）→ 窗口重开，按"本世代第一帧"记账
        if (s->in_use) g_replay_resync++;      // 仅"重启重同步"计数，新源不计
        s->in_use   = true;
        s->src_id   = src_id;
        s->epoch    = epoch;
        s->last_seq = seq;
        s->bits[0]  = 1u;            // 最低位置 1，标记该 seq 已见
        s->bits[1]  = 0;
        portEXIT_CRITICAL(&g_replay_mux);
        return true;
    }

    int16_t d = (int16_t)(seq - s->last_seq);
    if (d == 0 || (d < 0 && -d > 63)) {
        // 重复或过旧（超出 64 位窗口）
        g_replay_drop++;
        portEXIT_CRITICAL(&g_replay_mux);
        return false;
    }
    if (d < 0) {
        // 窗口内乱序：检查 (last_seq - seq) 位
        int bit = -d;
        if (s->bits[bit >> 5] >> (bit & 31) & 1) {
            g_replay_drop++;
            portEXIT_CRITICAL(&g_replay_mux);
            return false;
        }
        s->bits[bit >> 5] |= (1u << (bit & 31));
        portEXIT_CRITICAL(&g_replay_mux);
        return true;
    }
    if (d >= 64) {
        // 大幅前进：窗口整体推进，旧位全部滑出 64 位窗（等价重开窗口）
        replay_shift_left(s, d);
        s->last_seq = seq;
        s->bits[0] |= 1u;
        portEXIT_CRITICAL(&g_replay_mux);
        return true;
    }
    // 0 < d < 64：正常前进，先查该位（乱序曾标记过则重放），再前移 d 并置位
    if (s->bits[d >> 5] >> (d & 31) & 1) {
        g_replay_drop++;
        portEXIT_CRITICAL(&g_replay_mux);
        return false;
    }
    replay_shift_left(s, d);
    s->last_seq = seq;
    s->bits[0] |= 1u;
    portEXIT_CRITICAL(&g_replay_mux);
    return true;
}

uint32_t crypto_replay_drop_count() { return g_replay_drop; }

uint32_t crypto_replay_resync_count() { return g_replay_resync; }

// ---- 密钥管理（CLI / 网页用）----
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool crypto_set_key_hex(const char* hex) {
    if (!hex || strlen(hex) != CRYPTO_KEY_LEN * 2) return false;
    uint8_t key[CRYPTO_KEY_LEN];
    for (int i = 0; i < CRYPTO_KEY_LEN; i++) {
        int hi = hexval(hex[i * 2]);
        int lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = (uint8_t)((hi << 4) | lo);
    }

    cfg_t c;
    nvs_cfg_load(&c);
    memcpy(c.crypto_key, key, CRYPTO_KEY_LEN);
    nvs_cfg_save(&c);

    portENTER_CRITICAL(&g_key_mux);
    memcpy(g_key, key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);
    g_enabled = true;
    return true;
}

void crypto_disable() {
    portENTER_CRITICAL(&g_key_mux);
    memset(g_key, 0, sizeof(g_key));
    portEXIT_CRITICAL(&g_key_mux);
    g_enabled = false;

    cfg_t c;
    nvs_cfg_load(&c);
    memset(c.crypto_key, 0, CRYPTO_KEY_LEN);
    nvs_cfg_save(&c);
}
