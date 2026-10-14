#include "airtime.h"

uint32_t airtime_ms(uint32_t len_bytes, uint8_t sf, float bw_khz, uint8_t cr) {
    if (sf < 7 || sf > 12 || cr < 5 || cr > 8 || bw_khz <= 0.0f) return 0;

    uint32_t bw = (uint32_t)(bw_khz + 0.5f);
    if (bw == 0) return 0;

    // 符号时长 us = 2^SF / BW(kHz)
    uint32_t tsym_us = ((uint32_t)1 << sf) * 1000u / bw;

    // 低速率优化：SF11/SF12 且 BW125
    const uint32_t de  = (sf >= 11 && bw == 125) ? 1u : 0u;
    const uint32_t ih  = 0u;   // 显式头
    const uint32_t crc = 1u;   // CRC 开

    // 载荷符号数 = 8 + ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH) / (4*(SF - 2*DE))) * CR
    int32_t num = (int32_t)(8u * len_bytes) - 4 * (int32_t)sf + 28 + 16 * (int32_t)crc - 20 * (int32_t)ih;
    int32_t den = 4 * ((int32_t)sf - 2 * (int32_t)de);
    uint32_t n_payload = 8u;
    if (num > 0 && den > 0) {
        uint32_t n_sym = (uint32_t)((num + den - 1) / den);   // 向上取整
        n_payload += n_sym * cr;
    }

    // 总时间 = 符号时长 × (载荷符号 + 前导 8 + 帧同步 4.25)
    uint32_t us = tsym_us * (n_payload + 8u) + tsym_us * 17u / 4u;
    return (us + 999u) / 1000u;   // 向上取整到 ms
}
