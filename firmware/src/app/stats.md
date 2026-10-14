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
| 空口 | rssi snr link_quality% |
| 系统 | heap_free wdt_resets rf_reinit |

## LED 状态（WS2818 RGB @ GPIO48）
- 编码原则：**颜色 = 身份（角色），节奏 = 健康（链路）**；硬件故障整灯红覆盖角色色
- 完整颜色×节奏映射表见同目录 `led_states.md`

## 关键实现点
- `stats_task`（核1，优先级 4）：采集 → 更新 LED → `watchdog_feed()`
- 失联判定：`failsafe_s`（默认 3s）无上行 → 发 `EV_LINK_LOST`

## 对外接口
见 `stats.h`。

## 验收（P0）
- `stats` 命令每 1s 刷新，计数单调增长；LED 按状态正确闪
