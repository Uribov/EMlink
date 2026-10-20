# stats —— 统计 / LED / 喂狗

## 职责

每 1s 汇总各模块计数，刷新 LED，喂看门狗。

三件事共用一个 1s 周期，LED 和失联判定都挂在这次轮询上。


## 依赖

- `frame_codec`/`mac`/`mesh`/`frag` 的计数接口、`watchdog.h`、`pinmap.h`（LED）
- `../drivers/usb_cdc.h`（Z1：`stats_print()` 的输出走 USB CLI 出口 `usb_cli_write`）

依赖的都是只读的计数 getter，没有任何模块反向依赖 stats。


## 统计指标（架构 §10）

| 类 | 指标 |
|---|---|
| 链路 | tx_frames rx_frames ack_ok ack_miss retries |
| 网络 | fwd dropped_ttl dropped_dup dropped_crc |
| 分片 | reasm_timeout |
| 空口 | rssi snr link_quality%（P1-10：= ACK 成功率 `100*ack_ok/(ack_ok+ack_miss)`，还没有 ACK 样本时为 0；原为恒 0 的占位） |
| 系统 | heap_free wdt_resets rf_reinit |
| 丢弃分层 | P1-10 新增，CLI `status` 显示（未进 `stats_t`）：收包队列 `radio_rx_drop` / 射频请求 `radio_req_drop` / 发送队列 `rtos_tx_drop` / UART `uart_fc_drop` / USB `usb_cdc_drop` / 流控 `ratelimit_drop` |

> 与 CLI `status` 的关系：`status` 是分层视图（驱动/中间/应用，看「哪一层在丢」）；`stats` 打印的是本模块的原始计数行。
> 两边不完全重叠：`fwd` / `dup`(dropped_dup) / `reasm_to`(reasm_timeout) 目前只有 `stats` 打印（`status` 未覆盖）——所以别顺手删掉 `stats` 命令。
> 将来若把这三项并进 `status`，`stats` 命令即可删除（少一条路少一处坑）。

两份输出的分工：`status` 回答「哪一层在丢」，`stats` 回答「丢了多少」。


## LED 状态（WS2818 RGB @ GPIO48）

- 编码原则：颜色 = 身份（角色），节奏 = 健康（链路）；硬件故障整灯红覆盖角色色
- 完整颜色×节奏映射表见同目录 `led_states.md`

本模块只负责把状态喂给 LED，完整的颜色与节奏映射在 `led_states.md`。


## 关键实现点

- `stats_task`（核1，优先级 4）：任务开头 `watchdog_register_current()` 订阅 WDT → 采集 → 更新 LED → `watchdog_feed()`（喂自己）
- 失联判定：`failsafe_s`（默认 3s）无上行 → 发 `EV_LINK_LOST`
- 输出出口（Z1，2026-10-16 修）：`stats_print()` 必须走 `usb_cli_write()`（与 `cli_print` 同一个口）。
  绝不能用 `Serial1` —— 本工程 USB 只有一个 CDC（`Serial`），`Serial1` 是从未 `begin()` 的 UART1 硬件串口，输出会进黑洞。
  （这个 bug 从 P1-6 单口分流改造起存在：那次只动了 `usb_cdc.cpp/.h` + `main.cpp`（`cli.cpp` 明确未改），改完没有全仓搜「还有谁在用 `Serial1` 打印」，`stats_print` 落在 `app/` 里成了漏网的一处。）

第二条是踩过的坑：单口分流改造时漏了 stats 的打印出口，排查时先从「输出走了哪个口」查起。


## 对外接口

见 `stats.h`。

对外只有 `stats_start()`、`stats_collect()`、`stats_print()` 和灯态相关的几个入口。


## 验收（P0）

- 敲 `stats` 能在 CLI 上打印出计数行（`fwd/ttl/dup/reasm_to` + `rssi/snr/heap/wdt/rf_reinit`），计数单调增长
- LED 按状态正确变化（颜色=角色，整灯红=射频未就绪）

一条验数据通路，一条验灯。


---

## 当前实现架构（供人和子 agent 阅读）

### static 变量

- `g_led`：`Adafruit_NeoPixel`（WS2818 @ GPIO48）
- `g_failsafe_s`：失联阈值（NVS `failsafe_s`，默认 3）
- `g_link_was_lost`：失联边沿检测

三个变量都是模块内部状态，外部通过 getter 读。


### stats_task 每 1s 按序做的事

1. `watchdog_feed()`（先 `watchdog_register_current()` 订阅自己；loopTask 已不再订阅 WDT，见 `watchdog.md`）
2. `power_usb_present()`（内部做 USB 插拔检测并发 EV_USB_PLUG/UNPLUG）
3. 失联判定：`millis() - mesh_last_rx_ms() > failsafe_s*1000` → 置 EV_LINK_LOST；恢复 → EV_LINK_RESTORED（边沿触发）
4. 事件分发：读事件位 `(USB_PLUG|USB_UNPLUG|LINK_LOST|LINK_RESTORED)` → `failsafe_on_event()` → 清位
5. LED：颜色=角色，节奏=`failsafe_link_lost()?LOST:OK`；射频未就绪（`!radio_is_ready()`）时整灯红（`LED_HEALTH_FAULT`，覆盖角色色，P0-4）

五步严格按序执行，第一步喂狗保证后面几步即使变慢也不会被 WDT 判死。


### 关键点

- 事件消费统一收敛在 stats_task（1s 轮询），failsafe_policy 无独立任务，靠这里喂事件
- `stats_collect()` 从 mac/mesh/frag/watchdog/radio 的 getter 聚合计数（tx/rx/ack/fwd/drop/rssi/heap...）
- `stats_print()` 由 `cli_task` 的 `stats` 分支调用（按需打印一次，不是每秒自动刷屏），输出经 `usb_cli_write`（Z1）
- BEACON 发送已移走：原「GCS 每 1s 发 BEACON」改由 adr 模块统一管（5s 周期，SF9 锚点），本模块不再发

事件分发全部集中在 stats_task 一个地方，别的模块只负责置事件位。
