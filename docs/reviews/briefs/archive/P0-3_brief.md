# EMlink M0 修复 3/4：LBT（CAD）状态驱动轮询修复（P0-3）

## 背景
`radio_channelActivity()` 现在这样写：`startChannelScan()`（只是**启动** CAD，非阻塞）后立刻 `getChannelScanResult()` 读 IRQ 状态 —— CAD 还没跑完，读到 `RADIOLIB_ERR_UNKNOWN`（负数）；而 `mac_lbt()` 把 `act <= 0` 一律当"信道空闲" → **LBT 形同虚设（永远放行）**。

本次采用**状态驱动轮询**方案：轮询 IRQ 状态直到 CAD 完成（带超时）；超时/未知一律按"忙"处理（宁可退避，不要误发）。
（中断驱动方案不在本次实现，由调用方单独留档到 `radio-update.md`。）

## 只允许修改这 2 个文件
1. `firmware/src/drivers/radio.cpp`
2. `firmware/src/link/mac.cpp`

## 具体改动

### A. `radio.cpp`
#### A1. 新增两个 static 状态（放在 `g_suspended` 附近）
```c
// 当前调制参数（CAD 超时推导用；radio_init 设定、radio_set_sf 更新）
static uint8_t g_cur_sf = LORA_DEF_SF;
static float   g_cur_bw_khz = LORA_DEF_BW;
```
- `radio_init()` 成功路径里记录 `g_cur_sf = sf; g_cur_bw_khz = bw;`
- `radio_set_sf(sf)` 里 `setSpreadingFactor` 成功后更新 `g_cur_sf = sf;`

#### A2. 重写 `radio_channelActivity()`（保持签名与返回值语义：0=空闲 1=忙 负=错误）
```c
// CAD 侦听（LBT 用）：状态驱动轮询，直到 CAD 完成或超时（P0-3）
//   0 = 空闲（CAD_DONE）；1 = 忙（CAD_DETECTED）；负 = 错误/超时（调用方按“忙”处理）
int16_t radio_channelActivity() {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio || !g_radio_ready) return -1;

    // 启动 CAD（非阻塞：RadioLib 只把芯片设为 CAD 模式就返回）
    int16_t st = g_radio->startChannelScan();
    if (st != RADIOLIB_ERR_NONE) return st;

    // CAD 硬件耗时 ≈ 若干个符号周期 → 按当前 SF/BW 推导超时（符号时长 us = 2^SF / BW）
    // 5 个符号 + 3ms 余量；轮询期间 vTaskDelay(1) 让出 CPU（rf_task 优先级更高，不受影响）
    uint32_t tsym_us = ((uint32_t)1 << g_cur_sf) * 1000u / (uint32_t)g_cur_bw_khz;   // 例：SF7/125k → 1024us
    uint32_t timeout_ms = tsym_us * 5u / 1000u + 3u;                                  // 例：SF7 → 8ms，SF10 → 44ms
    uint32_t t0 = millis();
    while (millis() - t0 <= timeout_ms) {
        int16_t res = g_radio->getChannelScanResult();
        if (res == RADIOLIB_CHANNEL_FREE) return 0;                                  // CAD_DONE → 空闲
        if (res == RADIOLIB_LORA_DETECTED || res == RADIOLIB_PREAMBLE_DETECTED) return 1;  // CAD_DETECTED → 忙
        // RADIOLIB_ERR_UNKNOWN：CAD 尚未完成 → 继续轮询
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -2;   // 超时：状态未知 → 调用方按“忙”处理
#else
    return -1;
#endif
}
```
保留原有 `g_radio->startChannelScan()` / `getChannelScanResult()` 的用法（RadioLib 6.6 语义：前者启动、后者读结果），不要引入新 API。

#### A3. 顺带：`mac_send` 的两个长等待循环补喂狗（P0-1 机制配套，防误触发 5s WDT）
本文件不涉及；此项在 mac.cpp 做（见 B3）。

### B. `mac.cpp`
#### B1. 重写 `mac_lbt()` 的忙闲判定（核心修复）
```c
// LBT：最多 3 次，忙则随机退避 5~20ms；3 次仍不可用返回 false（硬发）
// 语义（P0-3）：0=空闲放行；1=忙、负=错误/超时（状态未知）→ 一律按“忙”处理，退避重试
static bool mac_lbt() {
    for (int i = 0; i < MAC_LBT_MAX_TRIES; i++) {
        int16_t act = radio_channelActivity();
        if (act == 0) return true;   // 信道空闲
        uint32_t wait = MAC_BACKOFF_MIN_MS + random(MAC_BACKOFF_MAX_MS - MAC_BACKOFF_MIN_MS + 1);
        vTaskDelay(pdMS_TO_TICKS(wait));
        watchdog_feed();             // 退避等待也喂狗（mac_send 只由 mac_task 调用）
    }
    return false;   // 3 次仍不可用 → 硬发（控制指令不可无限憋）
}
```
（`mac.cpp` 已 include `../service/watchdog.h`。）
#### B2. `mac_send()` 开头的占空比等待循环也补喂狗
```c
    while (!duty_reserve(airtime)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        watchdog_feed();             // 限流等待可能接近 1s，防 WDT 误触发
    }
```
#### B3. 不要动 mac_send 的 ACK 等待/重传逻辑、不要改 MAC_* 参数宏、不要改 mac_task 结构。

## 硬约束
- 只改这 2 个文件；不要动 radio.h / mac.h / 任何 .md / 其他模块；
- 不改 CAD 相关的 RadioLib 调用方式，不引入新依赖；
- 中文注释，风格一致；最小 diff；
- **不要运行编译命令**、**不要 git commit**（验证由调用方做）。

## 完成后请输出
1. `git diff --stat`；
2. 3~5 行中文总结。
