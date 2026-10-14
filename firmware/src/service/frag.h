#pragma once
// ============================================================
// frag —— MAVLink 大包分片 / 重组 / 缺件 NACK
// 依赖：frame_codec.h（FRAME_MAX_PAYLOAD=180）；约定见架构 §6
// ============================================================
#include <Arduino.h>
#include "../link/frame_codec.h"   // FRAME_MAX_PAYLOAD / FLAG_FRAG

// 分片头（对应帧偏移 9~12 的 frag_hdr[4]）
struct frag_hdr_t {
    uint8_t msg_id;      // 本组标识
    uint8_t frag_index;  // 第几片（0 起）
    uint8_t frag_total;  // 总片数
    uint8_t frag_seq;    // 组序号（区分不同大消息）
};

#define FRAG_REASM_TIMEOUT_MS 2000

void frag_init();

// 发送侧：len 超过阈值时拆片；返回片数（=1 表示无需拆）
// out 每片 <= FRAME_MAX_PAYLOAD；hdr 输出公共分片头
size_t frag_split(const uint8_t* data, size_t len,
                  uint8_t out[][FRAME_MAX_PAYLOAD], size_t max_frags,
                  frag_hdr_t* hdr);

// 接收侧：喂入一片，拼齐后回调整包；重组键 =(orig_sysid, frag_seq)，多机并发不串
void frag_reassemble(uint8_t orig_sysid, const frag_hdr_t* hdr, const uint8_t* data, size_t len,
                     void (*done)(const uint8_t* full, size_t len));

// 统计
uint32_t frag_timeout_count();
