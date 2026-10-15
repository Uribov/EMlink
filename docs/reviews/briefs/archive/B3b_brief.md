# EMlink 任务书 B-3b：令牌桶精度修复 + init 接线（B-3a 的补丁）

## 背景（两个必须修的问题）
1. **补充令牌的整数除法把余量丢光**：现在是 `g_ctrl_tokens += (uint32_t)(dt * RL_CTRL_RESERVE_PCT / 100);`。
   `mac_send` 的调用间隔 `dt < 10ms` 时（**空口忙、高频发包时正是限流最该起作用的时刻**），`dt*10/100` 恒为 0 → **控制桶永远补不回来**；`dt = 1ms` 时用户桶（90%/100）同样恒为 0 → 限流在最需要的时候失效。
2. **`ratelimit_init()` 没有任何调用点**（上一条任务书漏了），启动只靠 static 零初始化。

## 允许改动的文件（其它一律不动）
1. `firmware/src/service/ratelimit.h`、`firmware/src/service/ratelimit.cpp`
2. `firmware/src/link/mac.cpp`

**不要动**：任何 `.md`、`mac.h`、`radio.*`、`rtos.*`、`cli.cpp`、`main.cpp`；不要跑 pio/编译；不要 git commit/add。

## 1. 余量累加：消除整数除法损失
`ratelimit.cpp` 新增两个"未满 1 令牌的余量"累加器（单位：**百分之一令牌**）：
```c
static uint32_t g_ctrl_frac = 0;   // 控制桶未满 1 令牌的余量（0.01 令牌为单位）
static uint32_t g_user_frac = 0;
```
把 `rl_refill()` 改成：
```c
static void rl_refill(uint32_t now) {
    uint32_t dt = now - g_last_ms;
    if (dt == 0) return;
    g_last_ms = now;

    // 不要写成 dt*PCT/100：mac_send 高频调用（dt<10ms）时整数除法会把余量全丢，
    // 控制桶永远补不回来 —— 而那正是限流最该起作用的时刻。这里用余量累加。
    g_ctrl_frac   += dt * RL_CTRL_RESERVE_PCT;           // dt*10 → 百分之一令牌
    g_ctrl_tokens += g_ctrl_frac / 100;
    g_ctrl_frac   %= 100;

    g_user_frac   += dt * (100 - RL_CTRL_RESERVE_PCT);   // dt*90
    g_user_tokens += g_user_frac / 100;
    g_user_frac   %= 100;

    if (g_ctrl_tokens > RL_CTRL_BURST_MS) g_ctrl_tokens = RL_CTRL_BURST_MS;
    if (g_user_tokens > RL_USER_BURST_MS) g_user_tokens = RL_USER_BURST_MS;
}
```
`ratelimit_init()` 里把 `g_ctrl_frac` / `g_user_frac` 一并清零。

## 2. 接线 `ratelimit_init()`
在 `mac.cpp` 的 `mac_start()` **第一行**调用：
```c
void mac_start() {
    ratelimit_init();   // P1-10：令牌桶是发送路径的准入层，必须在任何发送之前初始化
    ...
```
（`mac.cpp` 已 include `../service/ratelimit.h`，无需再加。）

## 3. 自检（报告里给结论）
1. 修复前，`dt = 5ms` 时控制桶**每秒**能补多少令牌？修复后呢？（两个数都算出来）
2. 余量累加器用 `uint32_t` 会不会溢出？（按 dt 的实际量级说明）
3. `ratelimit_init()` 为什么放在 `mac_start()` 而不是 `main.cpp`？

## 完成后报告
1. 改动摘要（原样 vs 新样子）
2. 上面 3 条自检结论
3. 本次改动**不能**保证哪些性质
