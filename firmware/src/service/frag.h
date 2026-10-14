#pragma once
// ============================================================
// frag —— MAVLink 大包分片 / 重组 / 缺件 NACK
// 依赖：frame_codec.h；约定见架构 §6
// 片大小用 FRAME_MAX_PLAIN（175，= 密文上限 179 - tag 4B）：
//   无论是否加密都按明文上限切，加密后每片密文 179 正好塞进 payload 区。
// ============================================================
#include <Arduino.h>
#include "../link/frame_codec.h"   // FRAME_MAX_PLAIN / FLAG_FRAG

// 分片头（对应帧偏移 11~14 的 frag_hdr[4]）
struct frag_hdr_t {
    uint8_t msg_id;      // 本组标识
    uint8_t frag_index;  // 第几片（0 起）
    uint8_t frag_total;  // 总片数
    uint8_t frag_seq;    // 组序号（区分不同大消息）
};

#define FRAG_REASM_TIMEOUT_MS 2000
#define FRAG_NACK_WAIT_MS     500    // 收第一片后 500ms 还没集齐 → 发 NACK 催缺件

void frag_init();

// 发送侧：len 超过阈值时拆片；返回片数（=1 表示无需拆）
// out 每片 <= FRAME_MAX_PLAIN；hdr 输出公共分片头
size_t frag_split(const uint8_t* data, size_t len,
                  uint8_t out[][FRAME_MAX_PLAIN], size_t max_frags,
                  frag_hdr_t* hdr);

// 接收侧：喂入一片，拼齐后回调整包；重组键 =(src_id, orig_sysid, frag_seq)，多机并发不串
// src_id = 发片源节点（缺件时回 NACK 用）
void frag_reassemble(uint8_t src_id, uint8_t orig_sysid, const frag_hdr_t* hdr, const uint8_t* data, size_t len,
                     void (*done)(const uint8_t* full, size_t len));

// 周期检查缺件（由 mesh_task 每 ~100ms 调）：超时未拼齐 → 发 NACK 催缺件
void frag_tick();

// 统计
uint32_t frag_timeout_count();
uint32_t frag_slot_evict_count();   // 并发槽耗尽被迫淘汰次数
