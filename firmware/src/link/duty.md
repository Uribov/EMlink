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
