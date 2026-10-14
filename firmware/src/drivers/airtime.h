#pragma once
// ============================================================
// airtime —— LoRa 空中时间计算（Semtech 官方公式）
// 用途：ACK 超时 / 占空比预算 / 发送超时 全部由本模块推导，禁止再写经验粗估。
// 依赖：无（纯整数计算）；规格：airtime.md
// ============================================================
#include <Arduino.h>

// 帧的空中时间（ms，向上取整）
//   len_bytes：帧总线上长度（EMlink = FRAME_HDR_LEN + payload + FRAME_CRC_LEN）
//   sf       ：7..12
//   bw_khz   ：125 / 250 / 500
//   cr       ：5..8（表示 4/5..4/8）
// 固定假设（与 radio_init 的 begin(..., preambleLength=8, ...) 一致）：
//   前导 8 符号 + 4.25 符号帧同步、显式头(IH=0)、CRC 开、低速率优化按 SF11&125kHz 自动开
// 参数非法返回 0。
uint32_t airtime_ms(uint32_t len_bytes, uint8_t sf, float bw_khz, uint8_t cr);
