# stats —— 统计 / LED / 喂狗

## 职责
每 1s 汇总各模块计数，刷新 LED，喂看门狗。

## 依赖
- `frame_codec`/`mac`/`mesh`/`frag` 的计数接口、`watchdog.h`、`pinmap.h`（LED）

## 统计指标（架构 §10）

| 类 | 指标 |
|---|---|
| 链路 | tx_frames rx_frames ack_ok ack_miss retries |
| 网络 | fwd dropped_ttl dropped_dup dropped_crc |
| 分片 | reasm_timeout |
| 空口 | rssi snr **link_quality%**（P1-10：= ACK 成功率 `100*ack_ok/(ack_ok+ack_miss)`，还没有 ACK 样本时为 0；原为恒 0 的占位） |
| 系统 | heap_free wdt_resets rf_reinit |
| 丢弃分层 | P1-10 新增，CLI `status` 显示（未进 `stats_t`）：收包队列 `radio_rx_drop` / 射频请求 `radio_req_drop` / 发送队列 `rtos_tx_drop` / UART `uart_fc_drop` / USB `usb_cdc_drop` / 流控 `ratelimit_drop` |

## LED 状态（WS2818 RGB @ GPIO48）
- 编码原则：**颜色 = 身份（角色），节奏 = 健康（链路）**；硬件故障整灯红覆盖角色色
- 完整颜色×节奏映射表见同目录 `led_states.md`

## 关键实现点
- `stats_task`（核1，优先级 4）：任务开头 `watchdog_register_current()` 订阅 WDT → 采集 → 更新 LED → `watchdog_feed()`（喂自己）
- 失联判定：`failsafe_s`（默认 3s）无上行 → 发 `EV_LINK_LOST`

## 对外接口
见 `stats.h`。

## 验收（P0）
- `stats` 命令每 1s 刷新，计数单调增长；LED 按状态正确闪

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_led`：`Adafruit_NeoPixel`（WS2818 @ GPIO48）
- `g_failsafe_s`：失联阈值（NVS `failsafe_s`，默认 3）
- `g_link_was_lost`：失联边沿检测

### stats_task 每 1s 按序做的事
1. `watchdog_feed()`（先 `watchdog_register_current()` 订阅自己；loopTask 已不再订阅 WDT，见 `watchdog.md`）
2. `power_usb_present()`（内部做 USB 插拔检测并发 EV_USB_PLUG/UNPLUG）
3. 失联判定：`millis() - mesh_last_rx_ms() > failsafe_s*1000` → 置 EV_LINK_LOST；恢复 → EV_LINK_RESTORED（边沿触发）
4. 事件分发：读事件位 `(USB_PLUG|USB_UNPLUG|LINK_LOST|LINK_RESTORED)` → `failsafe_on_event()` → 清位
5. LED：颜色=角色，节奏=`failsafe_link_lost()?LOST:OK`；**射频未就绪（`!radio_is_ready()`）时整灯红**（`LED_HEALTH_FAULT`，覆盖角色色，P0-4）

### 关键点
- 事件消费统一收敛在 stats_task（1s 轮询），failsafe_policy 无独立任务，靠这里喂事件
- `stats_collect()` 从 mac/mesh/frag/watchdog/radio 的 getter 聚合计数（tx/rx/ack/fwd/drop/rssi/heap...）
- BEACON 发送已移走：原"GCS 每 1s 发 BEACON"改由 adr 模块统一管（5s 周期，SF9 锚点），本模块不再发
