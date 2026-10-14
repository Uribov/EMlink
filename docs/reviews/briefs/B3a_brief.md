# EMlink 任务书 B-3a：按 QoS 的令牌桶流量整形（对应审查日志 P1-10 第 1 条）

## 背景（为什么改）
- QGC 默认会请求 10~50Hz 遥测流，链路**没有按消息类型的限速/丢弃策略**。
- `mac_send()` 在占空比超限时是**原地死等**（`while (!duty_reserve) vTaskDelay(10ms)`），等待期间**心跳/命令全部排队**，还把 `adr_tick()` 一并堵住 → 极端情况下心跳节拍被拉长到 1s+，逼近飞控的失联判定。
- 队列满时全部**静默丢**且无计数，现场无法诊断丢在哪一层。

**修法**：新增 `service/ratelimit`——**按 QoS 分桶的令牌桶**，计价单位 = **空中毫秒**（与 duty 同口径，用 `radio_airtime_ms()`）：
- **控制类（QOS2）有保底份额**（默认 10% 空口时间 = 10ms/s，突发上限 30ms）→ 心跳/命令永远不会被遥测挤下空口；
- **用户类（QOS0/QOS1）**拿剩余份额（90ms/s，突发上限 100ms）；
- **准入即丢、不阻塞**：超限直接丢并计数，不再死等（死等是"把控制流拖住"的根因）。

## 允许改动的文件（其它一律不动）
1. 新建 `firmware/src/service/ratelimit.h`、`firmware/src/service/ratelimit.cpp`
2. `firmware/src/link/frame_codec.h`（只加一个小内联取值函数）
3. `firmware/src/link/mac.h`、`firmware/src/link/mac.cpp`
4. `firmware/src/app/cli.cpp`
5. `firmware/src/link/adr.h`（**只改一行陈旧注释**，见下）

## 特别注意（顺带修一处陈旧注释）
`adr.h` 里 `// 周期 tick（由 stats_task 1s 调一次）；GCS 在周期起点发 BEACON + 切 SF` 与实际不符：`adr_tick()` 现在挂在 **mac_task 循环**里（`mac.cpp` 调用，每轮 ~5ms）。把这行注释改成与实现一致。

**不要动**：任何 `.md`、`duty.*`、`radio.*`、`mesh.*`、`rtos.*`、`main.cpp`；不要跑 pio/编译；不要 git commit/add。

## 1. 新建 `service/ratelimit.h`
```c
#pragma once
// ============================================================
// ratelimit —— 按 QoS 的令牌桶流量整形（P1-10）
// 计价单位 = 空中毫秒（airtime），与 duty 同口径；控制流有保底份额。
// ============================================================
#include <Arduino.h>

#define RL_CTRL_RESERVE_PCT   10   // 控制流(QOS2)保底份额（% 的每秒空口时间）
#define RL_CTRL_BURST_MS      30   // 控制桶容量（突发上限，ms）
#define RL_USER_BURST_MS     100   // 用户桶容量（ms）
#define RL_WINDOW_MS        1000   // 份额基准周期（ms，仅用于注释/换算）

void     ratelimit_init(void);

// 准入判定：true=放行（已扣令牌）；false=超限（调用方丢弃并计数，**不要重试/死等**）
bool     ratelimit_allow(uint8_t qos, uint32_t airtime_ms);

// 诊断
uint32_t ratelimit_drop_count(void);        // 因限流被丢的帧数（全部类）
uint32_t ratelimit_ctrl_drop_count(void);   // 其中控制类（>0 说明连保底份额都不够了）
uint32_t ratelimit_oversize_count(void);    // 单帧就超过桶容量而被放行的次数
```

## 2. 新建 `service/ratelimit.cpp`（要点）
- static：`g_ctrl_tokens` / `g_user_tokens`（`uint32_t`，单位 ms）、`g_last_ms`、三个计数
- `ratelimit_init()`：桶清 0、`g_last_ms = millis()`、计数清零
- **补充令牌**（按流逝时间连续补充，就是令牌桶而不是固定窗口）：
```c
static void rl_refill(uint32_t now) {
    uint32_t dt = now - g_last_ms;
    if (dt == 0) return;
    g_last_ms = now;
    // 速率换算：每秒 WINDOW 毫秒的 10% → dt 毫秒里补 dt*10/100 个"空中毫秒"令牌
    g_ctrl_tokens += (uint32_t)((uint64_t)dt * RL_CTRL_RESERVE_PCT / 100);
    g_user_tokens += (uint32_t)((uint64_t)dt * (100 - RL_CTRL_RESERVE_PCT) / 100);
    if (g_ctrl_tokens > RL_CTRL_BURST_MS) g_ctrl_tokens = RL_CTRL_BURST_MS;
    if (g_user_tokens > RL_USER_BURST_MS) g_user_tokens = RL_USER_BURST_MS;
}
```
- **准入**：
```c
bool ratelimit_allow(uint8_t qos, uint32_t airtime_ms) {
    if (airtime_ms == 0) return true;
    rl_refill(millis());

    // 单帧就超过桶容量（例：SF10 的 179B 分片 ≈1.64s > 100ms）：
    // 一律拒绝会把这些帧**永久丢光**（与 duty 的 P1-2 例外同构）→ 放行 + 计数
    if (qos == QOS2) {
        if (airtime_ms > RL_CTRL_BURST_MS) { g_oversize++; return true; }
        if (g_ctrl_tokens >= airtime_ms) { g_ctrl_tokens -= airtime_ms; return true; }
        g_ctrl_drop++; g_drop++;
        return false;
    }
    if (airtime_ms > RL_USER_BURST_MS) { g_oversize++; return true; }
    if (g_user_tokens >= airtime_ms) { g_user_tokens -= airtime_ms; return true; }
    g_drop++;
    return false;
}
```
（`QOS2` 来自 `frame_codec.h`，本模块 include 它——与 crypto/frag 同向，不违反分层。"控制类"=QOS2，QOS0/QOS1 都算用户类。）
- 三个计数访问器（只读，无锁；调用方在 mac_task 单线程上下文，`mac_send` 只在 mac_task 里跑）

## 3. `frame_codec.h`：加一个取值小工具
```c
// 从线上帧字节取 QoS（偏移 1 的高 2 位），免去调用方解码整帧
static inline uint8_t frame_qos_of(const uint8_t* buf) { return (uint8_t)((buf[1] >> 6) & 0x03); }
```
（放在 `frame_decode` 声明附近；不要改任何现有函数。）

## 4. `mac.h` / `mac.cpp`
### mac.h
- 在现有 `mac_result_t` **末尾**追加一个枚举值（**不要改动已有值/顺序**）：
```c
    MAC_DROP_RATE,   // 被流量整形丢弃（P1-10：不阻塞、由上层计数）
```
### mac.cpp
- 顶部 `#include "../service/ratelimit.h"`
- `mac_send()`：在算出 `airtime`（P1-2 的 `radio_airtime_ms(len)`）之后、进 LBT/占空比等待之前，插入准入：
```c
    // P1-10：按 QoS 的令牌桶（控制类有保底份额）。超限直接丢，绝不死等——
    // 死等会把心跳/命令排在遥测后面，还会堵住 adr_tick()
    if (!ratelimit_allow(frame_qos_of(frame), airtime)) {
        return MAC_DROP_RATE;
    }
```
  其余逻辑（LBT、duty、ACK 等待）**保持原样**。
- **不要**在 mac 里再计一次数（计数在 ratelimit 内部）。

## 5. `cli.cpp`：status 显示
把现有「占空比」那行改成同时报限流（**不要新增行**，status 仍十行）：
```c
        snprintf(line, sizeof(line), "占空比: 超限放行=%u 次  限流丢弃=%u 次（控制类 %u，超桶放行 %u）\r\n",
                 (unsigned)duty_oversize_count(), (unsigned)ratelimit_drop_count(),
                 (unsigned)ratelimit_ctrl_drop_count(), (unsigned)ratelimit_oversize_count());
        cli_print(line);
```
（`ratelimit.h` 若未 include 请补。）

## 6. 自检（报告里给结论）
1. 为什么控制类必须用**独立**的桶，而不是和遥测共用一个？
2. 为什么超限要"直接丢"而不是"等令牌"？（结合 `mac_send` 死等会堵住 `adr_tick` 说明）
3. 单帧 airtime 大于桶容量时为什么必须放行？不放行会发生什么？（举 SF10 179B 分片的例子）
4. 令牌速率 10% + 90% 与 duty 的 100ms/s 上限是什么关系？两层同时存在会不会互相打架？

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检结论
3. 本次改动**不能**保证哪些性质（诚实列出，例如"份额参数没实测""QOS1 与 QOS0 没有区分"）
