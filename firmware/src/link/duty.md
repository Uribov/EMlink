# duty —— 占空比限流

## 职责
限制每节点每秒空中发射时间（默认 10%），守法规、也给别的节点留空口。

## 依赖
- 无硬件依赖；被 `mac` 在发送前调用

## 算法
- 滑动窗口：记录最近 1s 内已累计的 airtime（`airtime = 帧长×符号时间`，LoRa 可算）
- `duty_reserve(airtime)`：已用 + 申请 ≤ 上限 → 放行并累加；否则返回 false，`mac` 层排队等待
- 每 1s 由 `stats_task` 或内部定时器重置窗口

## 空中时间估算（SF7/BW125/CR4/5，供参考）
`symbolTime = 2^SF / BW`；`airtime ≈ (preamble + ceil((8·len - 4·SF + 28 + 16·CRC)/ (4·(SF-2))) · (CR+4)) · symbolTime`
（RadioLib 有 `getTimeOnAir()`，直接用它）

## 对外接口
见 `duty.h`。

## 验收
- 持续满负荷发包时，实测 1s 空中占比 ≈ 配置上限，不超限

## 当前实现架构（供人和子 agent 阅读）

### 一句话
限制每节点每秒"在空中发东西"的总时长（默认 10%，即 1 秒里最多占 100ms），守法规、也给别的节点留空口。

### 实现（滑动窗口）
- `g_window_start` / `g_used_ms`：当前 1s 窗口的起点和已用空中时长
- `duty_reserve(airtime_ms)`：已用 + 申请 ≤ 上限 → 放行并累加；否则返回 false（mac 层排队等）
- 窗口过了 1s 自动重置

### 空中时长估算（当前是粗略值）
现在用 `len * 300 / 1000` 这种按字节长度比例估算（200B@SF7/BW125 ≈ 60ms）。
精确算法用 RadioLib `getTimeOnAir()`，但 duty 模块刻意不依赖 RadioLib（纯逻辑、可单测），所以先用公式估算，后续要精确再换。

### 调用链
mac_send 发送前先 `duty_reserve()`，超限就 `vTaskDelay(10ms)` 循环等。
