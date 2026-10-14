# EMlink 任务书 B-1：射频过流保护（OCP）放宽（对应审查日志 P1-11 第 1 条）

## 背景（为什么改）
RadioLib 的 `begin()` 内部会把 OCP（过流保护）设成 **60mA**，而 +22dBm 发射需要约 **120~140mA**（Semtech 建议 140mA）。本工程**从未调用 `setCurrentLimit()`**。
后果：高功率发射时可能触发芯片过流保护 → 输出被限/波形失真，个别批次的模块甚至表现为"发射中断"。上板表现为"功率设得越高反而越不靠谱"。

修法：**功率 > 14dBm 时把 OCP 放宽到 140mA**；≤14dBm 保持库默认 60mA（不无谓放宽，省电也少风险）。

**驱动选型本次不动**（保持 `SX1268` 类）——那条要空口 A/B 实测，已写进顶层 `todo.md` 等上板。

## 允许改动的文件（其它一律不动）
1. `firmware/src/drivers/radio.cpp`

**不要动**：任何 `.md`、`radio.h`、`pinmap.h`、`radios/*.h`；不要跑 pio/编译；不要 git commit/add。

## 改法（两处，都在 `begin()` 成功之后）

### 1) `radio_init()`：`begin(...)` 成功路径上
在 `st != RADIOLIB_ERR_NONE` 的失败分支之后（即初始化成功的路径上）加：
```c
    // P1-11：RadioLib 的 begin() 会把 OCP 设成 60mA，而 +22dBm 发射需 120~140mA
    // （Semtech 建议 140mA）。功率 > 14dBm 时放宽 OCP，避免高功率发射触发过流保护。
    // 必须在 begin() 之后调用——begin() 内部会把 OCP 设回 60mA。
    if (pwr > 14) g_radio->setCurrentLimit(140.0f);
```

### 2) `radio_retry_tick()`：`begin(...)` 成功分支内
在 `st == RADIOLIB_ERR_NONE` 分支里（`attachInterrupt(...)` 附近）加同一句，用重试参数：
```c
    if (g_init_pwr > 14) g_radio->setCurrentLimit(140.0f);
```

**注意**：`setCurrentLimit()` 返回 `int16_t`，直接调用即可（不接返回值不会告警）；不要为此新增变量，也不要改成会吞错误的写法。

## 自检（报告里给结论）
1. 为什么这两句必须放在 `begin()` **之后**？放在之前会怎样？
2. `radio_init` 里的 `pwr` 与重试路径里的 `g_init_pwr` 是不是同一个值？（读代码说明赋值链）
3. 功率 ≤ 14dBm 时 OCP 保持多少？为什么这样取舍？

## 完成后报告
1. 改动摘要（原样 vs 新样子）
2. 上面 3 条自检结论
3. 本次改动**不能**保证哪些性质
