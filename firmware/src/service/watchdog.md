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

## 当前实现架构（供人和子 agent 阅读）

### 一句话
程序跑飞、射频卡死时的"兜底"。两层：任务级看门狗 + 射频自救。

### 看门狗（任务级 SW WDT）
- `watchdog_init()`：`esp_task_wdt_init(5s, panic)` + 把 loopTask 加进 WDT
- `watchdog_feed()`：stats_task 每 1s 喂一次
- 5s 不喂 → 触发 panic（架构 §8.2 第 4 条）

### 射频自救（架构 §8.2 第 3 条）
```
watchdog_rf_fault() 被 radio 调用（连续 3 次发送失败 / init 失败）
  ├─ 第 1、2 次：radio_hardReset()（拉 NRST 软复位射频芯片）+ rf_reinits++
  └─ 第 3 次：esp_restart() 整机重启 + wdt_resets++
```

### 触发源（radio.cpp）
- `radio_transmit` 连续 3 次失败 → `watchdog_rf_fault()`
- `radio_init` 失败 → `watchdog_rf_fault()`

### 统计接口
`watchdog_wdt_resets()` / `watchdog_rf_reinits()`，stats 采集显示
