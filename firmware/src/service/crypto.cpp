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
}

bool crypto_enabled() { return g_enabled; }

// nonce[13] 布局：前 10B 全 0（留扩展），[10]=src_id，[11..12]=seq 小端
// (src_id, seq) 唯一标识一帧，防 nonce 重放；转发不改这俩字段，故可原样转发密文。
static void build_nonce(uint8_t nonce[CRYPTO_NONCE_LEN], uint8_t src_id, uint16_t seq) {
    memset(nonce, 0, CRYPTO_NONCE_LEN);
    nonce[10] = src_id;
    nonce[11] = (uint8_t)(seq & 0xFF);
    nonce[12] = (uint8_t)((seq >> 8) & 0xFF);
}

bool crypto_seal_frame(frame_t* f, uint8_t* scratch, size_t scratch_cap) {
    if (!f || !g_enabled || !scratch) return false;
    if (f->payload_len == 0) return false;                 // 空 payload 不加密
    size_t total = f->payload_len + FRAME_TAG_LEN;
    if (total > FRAME_MAX_PAYLOAD || scratch_cap < total) return false;

    uint8_t nonce[CRYPTO_NONCE_LEN];
    build_nonce(nonce, f->src_id, f->seq);

    // 加锁拷 key 到局部（setkey 是耗时操作，锁只护 16B 拷贝，不护整个加解密）
    uint8_t key[CRYPTO_KEY_LEN];
    portENTER_CRITICAL(&g_key_mux);
    memcpy(key, g_key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int r = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (r == 0) {
        // 输出布局：密文（与明文等长）+ tag 4B；AAD 空（帧头完整性靠 CRC，防窃听是首要目标）
        r = mbedtls_ccm_encrypt_and_tag(&ctx, f->payload_len, nonce, CRYPTO_NONCE_LEN,
                                        nullptr, 0, f->payload, scratch,
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
    build_nonce(nonce, f->src_id, f->seq);

    uint8_t key[CRYPTO_KEY_LEN];
    portENTER_CRITICAL(&g_key_mux);
    memcpy(key, g_key, CRYPTO_KEY_LEN);
    portEXIT_CRITICAL(&g_key_mux);

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int r = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (r == 0) {
        r = mbedtls_ccm_auth_decrypt(&ctx, clen, nonce, CRYPTO_NONCE_LEN,
                                     nullptr, 0, f->payload, out,
                                     f->payload + clen, FRAME_TAG_LEN);
    }
    mbedtls_ccm_free(&ctx);
    return (r == 0) ? clen : 0;
}

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
