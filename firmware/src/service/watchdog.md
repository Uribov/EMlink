# watchdog —— 双看门狗 + 射频自救

## 职责
程序跑飞、射频卡死时的兜底（架构 §8.2 第 3/4 条）。

## 依赖
- `esp_task_wdt`（任务级）+ 硬件看门狗定时器
- `radio.h`（`radio_hardReset`）
- 被 `stats_task`（周期喂狗）、`radio`（故障上报）使用

## 射频自救流程
1. `radio` 检测到连续 N 次 SPI 无响应/发送失败 → 调 `watchdog_rf_fault()`
2. 先 `radio_hardReset()`（NRST 拉低），计 `rf_reinits`
3. 复位后仍连续故障 → 触发整机复位（重启），计 `wdt_resets`

## 喂狗策略
- `stats_task` 每 1s 喂一次任务 WDT；各长任务（mac/mesh/mavlink）注册到任务 WDT
- HW WDT 兜底更深层死机（任务调度都停时）

## 对外接口
见 `watchdog.h`。

## 验收（P4）
- 人为卡死射频 SPI → 自动 `rf_reinit`，射频恢复
- 人为死循环某任务 → SW WDT 复位该任务，整机不崩
