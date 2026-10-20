# watchdog —— 双看门狗 + 射频自救

## 职责

程序跑飞、射频卡死时的兜底（架构 §8.2 第 3/4 条）。

两层各管一类故障：任务级看门狗管「某个任务卡死」，射频自救管「芯片不响应」。


## 依赖

- `esp_task_wdt`（任务级）+ 硬件看门狗定时器
- `radio.h`（`radio_hardReset`）
- 被 rf/mac/mesh/mavlink/stats 各任务（各自订阅 + 各自喂狗）、`radio`（故障上报）使用

被所有任务依赖，但不依赖任何任务。


## 射频自救流程

1. `radio` 检测到连续 N 次 SPI 无响应/发送失败 → 调 `watchdog_rf_fault()`
2. 先 `radio_hardReset()`（NRST 拉低），计 `rf_reinits`
3. 复位后仍连续故障 → 触发整机复位（重启），计 `wdt_resets`

先软复位芯片，两次不成就重启整机，是逐级升级的。

---

## 喂狗策略（P0-1 修正 · 方案 A）

- 不订阅 loopTask（Arduino 的 loopTask 只有调 `enableLoopWDT()` 才会喂狗，本工程没调，曾导致上板 5s 复位循环）
- 5 个关键任务各自订阅、各自喂：`rf_task`（等 DIO1 通知带 1s 超时）、`mac_task`（含 `mac_send` 等 ACK 的轮询内）、`mesh_task`、`mavlink_task`、`stats_task`
- 任意订阅任务 5s 不喂 → panic 复位；IDF 的中断看门狗（INT_WDT）常开，兜底 ISR 长时间占用

订阅是「谁卡死谁负责」，不订阅 loopTask 是因为它从来不会卡。


## 对外接口

见 `watchdog.h`。

对外只有初始化、订阅、喂狗和两个计数函数。

---

## 验收（P4）

- 人为卡死射频 SPI → 自动 `rf_reinit`，射频恢复
- 人为死循环某任务 → SW WDT 复位该任务，整机不崩

两条分别验射频层和任务层。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

程序跑飞、射频卡死时的「兜底」。两层：任务级看门狗 + 射频自救。


### 看门狗（任务级 SW WDT，方案 A）

- `watchdog_init()`：只做 `esp_task_wdt_init(5s, panic)`（IDF 4.4 语义：重复调用=更新配置，返回 OK），不订阅任何任务
- `watchdog_register_current()`：任务自己订阅自己（`esp_task_wdt_add(nullptr)`；重复订阅 / 未初始化静默忽略，不打印）
- `watchdog_feed()`：喂的是当前任务（`esp_task_wdt_reset()`），必须由已订阅任务自己调用
- 订阅者：rf / mac / mesh / mavlink / stats；5s 不喂 → 触发 panic（架构 §8.2 第 4 条）

初始化和订阅分开，是因为订阅必须发生在各自任务内部。


### 射频自救（架构 §8.2 第 3 条）

```
watchdog_rf_fault() 被 radio 调用（连续 3 次发送失败 / init 失败）
  ├─ 第 1、2 次：radio_hardReset()（拉 NRST 软复位射频芯片）+ rf_reinits++
  └─ 第 3 次：esp_restart() 整机重启 + wdt_resets++
```

计数分两个：`rf_reinits` 记芯片复位次数，`wdt_resets` 记整机重启次数。


### 触发源（radio.cpp）

- `radio_transmit` 连续 3 次失败 → `watchdog_rf_fault()`
- `radio_init` 失败 → `watchdog_rf_fault()`（仅在曾经成功过的情况下，见 radio.md）


### 统计接口

`watchdog_wdt_resets()` / `watchdog_rf_reinits()`，stats 采集显示
