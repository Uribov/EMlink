#pragma once
// ============================================================
// crypto —— 帧载荷加密/解密（AES-128-CCM）
// 只加密 payload，帧头不加密（要能路由/转发）。
// 依赖：frame_codec.h（FRAME_TAG_LEN/FRAME_MAX_PLAIN）、nvs_cfg.h（密钥）
// 规格：crypto.md
// ============================================================
#include <Arduino.h>
#include "../link/frame_codec.h"

// AES-128 密钥 16 字节、CCM nonce 13 字节（tag 长度见 frame_codec.h FRAME_TAG_LEN）
#define CRYPTO_KEY_LEN    16
#define CRYPTO_NONCE_LEN  13

// 初始化：从 NVS 读密钥，判定是否启用（密钥非全 0 即启用）
void crypto_init();

// 是否启用加密
bool crypto_enabled();

// 加密 frame_t 的 payload：明文 -> 密文+tag（写入 scratch，容量 >= payload_len+TAG）。
// 成功后更新 f->payload 指向密文、payload_len += FRAME_TAG_LEN、f->enc = true。
// 未启用 / 空 payload 返回 false（调用方按明文原样处理）。
bool crypto_seal_frame(frame_t* f, uint8_t* scratch, size_t scratch_cap);

// 解密 frame_t 的 payload：密文+tag -> 明文（写入 out，容量 >= payload_len）。
// 返回明文长度；认证失败 / 未启用 / 非加密帧返回 0（调用方丢弃）。
size_t crypto_open_frame(const frame_t* f, uint8_t* out, size_t out_cap);

// 设置密钥（32 个 hex 字符 -> 16 字节，存 NVS，立即启用）
bool crypto_set_key_hex(const char* hex);

// 关闭加密（清密钥 + 写 NVS）
void crypto_disable();

// 本机发送时写入帧头的开机世代号（crypto_init 内取，见下）
uint8_t crypto_enc_epoch(void);

// 收端重放检查：成功返回 true（并记账）；重放/过旧返回 false
bool crypto_check_replay(uint8_t src_id, uint16_t seq);

// 重放丢弃计数（诊断用）
uint32_t crypto_replay_drop_count(void);
