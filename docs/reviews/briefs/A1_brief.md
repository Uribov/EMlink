# EMlink 任务书 A-1：时延基准修正（对应审查日志 P1-2）

## 背景（为什么改）
`link/mac.cpp` 第 56 行用 `airtime = 5 + len * 300 / 1000` 粗估空中时间（200B ≈ 65ms）。
实测（Semtech 官方 ToA 公式，BW125/CR4/5/前导 8/显式头/CRC 开）真实值是它的 **5~28 倍**：

| 帧 | SF7 | SF10 | 代码粗估 |
|---|---|---|---|
| 16B（ACK/信标） | 51.5 ms | 329.7 ms | 9.8 ms |
| 179B（分片满帧） | 287.0 ms | 1640.4 ms | 58.7 ms |
| 195B/200B（整帧上限） | 312.6 ms | 1804.3 ms | 63.5 ms |

后果两条：① 占空比限流器账目错 5~28 倍（形同虚设、且实际可能违规）；② ACK 超时写死 300ms，在 SF10 下 ACK 帧自身就要 330ms 空中时间 → **必然超时**，ADR 的 SF10 窗口内重传全部白烧。

修法：新增 ToA 计算模块，**ACK 超时 / 占空比预算 / 发送超时语义**三处全部由它推导。

## 硬性约束
- **只允许改/建下面列出的文件**，其它文件一律不碰。
- **不要动任何 `.md` 文档**（文档我来同步）。
- **不要运行编译命令**（`pio run`），**不要 `git commit`**。
- 保持现有代码风格：中文注释、模块内 `static`、`// ====` 分隔头。
- 不改任何函数名/签名，除了本任务书明确要求的。

## 允许改动的文件
1. 新建 `firmware/src/link/airtime.h`、`firmware/src/link/airtime.cpp`
2. `firmware/src/drivers/radio.h`、`firmware/src/drivers/radio.cpp`
3. `firmware/src/link/duty.h`、`firmware/src/link/duty.cpp`
4. `firmware/src/link/mac.h`、`firmware/src/link/mac.cpp`
5. `firmware/src/app/cli.cpp`

---

## 1. 新建 `link/airtime.h` / `airtime.cpp`

文件名 `airtime`，纯计算模块（不依赖 radio / RadioLib，只依赖 Arduino.h）。

```c
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
```

`airtime.cpp` 的实现（**严格按下面这段，整数运算，不要改成浮点近似**）：

```c
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
```

自检（不要写进代码，只用于你自己核对思路）：`airtime_ms(16,7,125,5)` 应为 52；`airtime_ms(16,10,125,5)` 应为 330；`airtime_ms(195,7,125,5)` 应为 313。

## 2. `drivers/radio.h` / `radio.cpp`

**(a) 去掉 `radio_transmit` 的第三个参数**（RadioLib 6.6 的 `transmit(data, len, addr)` 第三参是 **addr** 不是超时，传 1000 被静默忽略；SX126x 内部自管 5×ToA 超时）：

- `radio.h`：声明改为 `int16_t radio_transmit(const uint8_t* data, size_t len);`
  并在注释里写明：`// 注：RadioLib 的 transmit 第三参是 addr 而非超时，本接口不带超时参数；发送超时由 RadioLib 内部按 5×ToA 自管`
- `radio.cpp`：函数定义同步去掉形参与 `(void)timeout_ms;`

**(b) 新增当前调制参数的空中时间接口**（供 mac 推算，SF/BW/CR 的知识只留在 radio 内部）：

- `radio.h` 新增声明：
```c
// 按当前实际调制参数（SF/BW/CR）算一帧 len_bytes 字节的空中时间（ms）；射频未就绪/参数未知返回 0
uint32_t radio_airtime_ms(size_t len_bytes);
```
- `radio.cpp`：
  - 顶部 `#include "../link/airtime.h"`
  - 在 `g_cur_sf` / `g_cur_bw_khz` 旁边新增 `static uint8_t g_cur_cr = LORA_DEF_CR;`
  - `radio_init()` 成功分支里（`g_cur_sf = sf;` 那两行旁）补 `g_cur_cr = cr;`
  - `radio_retry_tick()` 重试成功分支里补齐三个当前值：`g_cur_sf = g_init_sf; g_cur_bw_khz = g_init_bw; g_cur_cr = g_init_cr;`（现状是重试成功后没更新，属顺手修正）
  - 实现：
```c
uint32_t radio_airtime_ms(size_t len_bytes) {
    if (!g_radio_ready || g_cur_cr < 5) return 0;
    return airtime_ms((uint32_t)len_bytes, g_cur_sf, g_cur_bw_khz, g_cur_cr);
}
```

## 3. `link/duty.h` / `duty.cpp`

新增"单帧就超预算"的放行保护 + 计数（否则 SF10 的 179B 帧 ≈1.6s 永远等不到预算，`mac_send` 会死等）：

- `duty.cpp`：`duty_reserve()` 开头插入
```c
    // 单帧空中时间就超过 1s 预算（例：SF10 的 179B 分片 ≈1.64s）：若一律拒绝，mac_send 会无限等待 → 放行 + 计数告警
    if (airtime_ms >= g_max_ms) { g_oversize++; return true; }
```
  新增 `static uint32_t g_oversize = 0;` 与
```c
uint32_t duty_oversize_count(void) { return g_oversize; }
```
- `duty.h`：新增 `uint32_t duty_oversize_count(void);`，注释里说明"单帧超预算 → 放行并计数"。

## 4. `link/mac.h` / `mac.cpp`

**(a) `mac.h`**：
- 保留 `#define MAC_ACK_TIMEOUT_MS 300`，但注释改为：`// ACK 超时下限（NVS 默认）；实际超时 = max(本值, ACK 帧空中时间 + MAC_ACK_GUARD_MS)`
- 新增 `#define MAC_ACK_GUARD_MS 50`，注释：`// ACK 帧空中时间之外的余量：收方处理 + 排队 + 射频切换`

**(b) `mac.cpp` `mac_send()`**：
- 删掉旧的 `uint32_t airtime = 5 + (uint32_t)len * 300 / 1000;`，改为：
```c
    // 占空比限流：空中时间按当前 SF/BW/CR 精确推导（P1-2）；未就绪时按满预算记账（最保守）
    uint32_t airtime = radio_airtime_ms(len);
    if (airtime == 0) airtime = DUTY_MAX_MS_PER_SEC;
```
- ACK 超时改为动态推导（放在 `int attempts = ...` 之前）：
```c
    // ACK 超时按 ACK 帧自身空中时间推导（SF10 下 ACK 帧就要 330ms，写死 300ms 必然超时）
    // NVS 配置值作为下限，只放大不缩小
    uint32_t ack_to = radio_airtime_ms(MAC_ACK_FRAME_LEN) + MAC_ACK_GUARD_MS;
    if (ack_to < g_ack_timeout_ms) ack_to = g_ack_timeout_ms;
```
  并在文件内常量区新增：
```c
// ACK 帧线上长度：仅帧头 + CRC（无 payload）
#define MAC_ACK_FRAME_LEN (FRAME_HDR_LEN + FRAME_CRC_LEN)
```
- 等 ACK 的循环里 `millis() - start < g_ack_timeout_ms` 改为 `< ack_to`
- `radio_transmit(frame, len, 1000)` 改为 `radio_transmit(frame, len)`

## 5. `app/cli.cpp`

`status` 分支里，紧随现有 `RF: ...` 那一行之后追加一行占空比健康度输出：
```c
        snprintf(line, sizeof(line), "占空比: 超限放行=%u 次\r\n", (unsigned)duty_oversize_count());
        cli_print(line);
```
若文件里还没有 `#include "../link/duty.h"` 则补上。

## 完成后请报告
- `git diff --stat` 输出
- 每个文件改了什么（中文简述）
- 你自检 `airtime_ms()` 三个样例值（16B/SF7、16B/SF10、195B/SF7）的推算结果
