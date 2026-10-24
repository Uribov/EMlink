#pragma once
// ============================================================
// version —— 固件版本 / 协议版本宣告（T-2）
// ============================================================
#include <Arduino.h>

// ---- 版本 ----
// 固件版本：手维护（改功能时手工 +0.0.1；自动从 git 生成版本号的办法记在 ROADMAP.md，现阶段不做）
#define EM_FW_VERSION      "0.3.0"

// 协议版本（空口帧格式 / 封装语义的兼容性编号）：
//   v1 = 14 字节帧头（A-5 之前）
//   v2 = 15 字节帧头（新增 enc_epoch）+ AEAD(AAD/nonce/replay window)
//   v3 = BEACON payload 从 1 字节扩到 2 字节（第 2 字节 = 发送者角色），
//        用于 AIR 低频"在场信标"；旧固件会报版本不一致，这是设计意图（需两端一起升级）。
// ★ 凡是改动帧格式、字段含义、封装流程，必须在这里 +1；两端不一致时的表现见 version.cpp 的说明。
#define EM_PROTO_VERSION   3

// 固件版本字符串（"0.1.0"）、协议版本字符串（"v3"）
const char* version_fw_str(void);
const char* version_proto_str(void);
