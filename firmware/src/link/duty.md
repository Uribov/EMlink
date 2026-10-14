# duty —— 占空比限流

## 职责
限制每节点每秒空中发射时间（默认 10%），守法规、也给别的节点留空口。

## 依赖
- 无硬件依赖；被 `mac` 在发送前调用
- 空中时间**不由本模块估算**：由调用方（`mac`）用 `radio_airtime_ms()` 算好传入（P1-2）

## 算法
- 滑动窗口：记录当前 1s 窗口内已累计的 airtime
- `duty_reserve(airtime_ms)`：已用 + 申请 ≤ 上限 → 放行并累加；否则返回 false，`mac` 层排队等待
- 窗口过了 1s 自动重置
- **单帧超预算例外**（P1-2）：当 `airtime_ms >= 上限`（例：SF10 的 179B 分片 ≈1.64s > 100ms）——一律拒绝会让 `mac_send` **无限等** → **放行 + 计数告警**（`duty_oversize_count()`）。这是"合规 vs 可用性"的取舍，计数在 CLI `status` 里可见

## 对外接口
见 `duty.h`：`duty_init / duty_reserve / duty_oversize_count`（上限是编译期常量 `DUTY_MAX_MS_PER_SEC`，无运行时 setter）

## 验收
- 持续满负荷发包时，实测 1s 空中占比 ≈ 配置上限，不超限
- SF10 下大分片不会把发送卡死（`duty_oversize_count` 递增，帧照发）

## 当前实现架构（供人和子 agent 阅读）

### 一句话
限制每节点每秒"在空中发东西"的总时长（默认 10%，即 1 秒里最多占 100ms），守法规、也给别的节点留空口。

### 实现（滑动窗口）
- `g_max_ms`（默认 `DUTY_MAX_MS_PER_SEC` = 100）、`g_window_start`、`g_used_ms`、`g_oversize`（新增计数）
- `duty_reserve(airtime_ms)` 顺序：
  1. `airtime_ms >= g_max_ms` → `g_oversize++` 并**放行**（单帧就超预算，拒绝＝死等）
  2. 窗口过期（≥1s）→ 重置起点与已用量
  3. 已用 + 申请 > 上限 → 返回 false（mac 层 `vTaskDelay(10ms)` 循环等，期间喂狗）
  4. 否则累加并放行

### 空中时长从哪来（P1-2 修正）
旧实现按字节长度粗估：`5 + len*300/1000`（200B 算 65ms，真实 SF7 是 318ms、SF10 是 1846ms，**差 5~28 倍**）→ 预算账本形同虚设。
现在 `mac_send` 用 `radio_airtime_ms(len)`（`drivers/airtime`，Semtech 公式）算好传入；射频未就绪返回 0 时，按满预算 100ms 记账（最保守）。
本模块仍是纯逻辑、零依赖、可主机单测。

### 调用链
`mac_send()` → `radio_airtime_ms(len)` → `duty_reserve(airtime)`：超限则 `vTaskDelay(10ms)` 等下一轮（每轮喂狗）。
