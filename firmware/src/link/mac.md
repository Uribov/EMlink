# mac —— 信道接入

## 职责
决定「什么时候发、要不要回执、丢了多少次」。负责 LBT、ACK、重传、退避。

## 依赖
- `radio.h`（`radio_channelActivity` 做 CAD、`radio_transmit` 发包、`radio_airtime_ms` 算空中时间）
- `frame_codec.h`（解析 `ack_req` / `src_id` / `seq`）
- `duty.h`（占空比限流）
- `adr.h`（mac_task 循环顶部调 `adr_tick` 切 SF）
- `service/watchdog.h`（mac_task 订阅任务看门狗；mac_send 等 ACK 期间喂狗）
- 被 `mesh`（转发）、`mav_router`（业务帧）调用

## 发送流程（`mac_send`）
1. **占空比**：`radio_airtime_ms(len)` 算本帧空中时间 → `duty_reserve()`，超限循环等（P1-2：不再按帧长粗估）
2. **LBT**（P0-3 修正语义）：`radio_channelActivity()` 只有返回 **0（CAD 明确判定空闲）** 才放行；忙、错误、超时（状态未知）一律按"忙"处理 → 随机退避 5~20ms 再试，最多 3 次
3. 3 次仍忙 → **硬发**（控制指令不能无限憋，返回 `MAC_LBT_BUSY` 标记）
4. 发送 `radio_transmit(frame, len)`（无超时参数，见 `radio.md`），失败返回 `MAC_TX_ERR`
5. 若 `ack_req`：等 ACK，**超时 = max(NVS 配置值, ACK 帧空中时间 + 50ms)**（P1-2）→ 按退避 **100→300→700ms** 重传，最多 `MAC_MAX_RETRIES` 次
6. 全失败返回 `MAC_ACK_TIMEOUT`，计 `ack_miss`；成功计 `ack_ok`

## ACK 约定
- 收到 `ack_req=1` 且 `dst_id` 匹配自己或广播 → 回 ACK 帧（`FRAME_ACK`，echo `src_id`+`seq`）
- ACK 帧本身 `ack_req=0`，不走重传（避免 ACK 风暴）

## 占空比联动
发送前先 `duty_reserve(radio_airtime_ms(len))`，超限则排队等（见 `duty` 模块）。

## 对外接口
见 `mac.h`。

## 验收（P1）
- 2 节点 500m 视距 0 丢包（`ack_miss=0`）
- 人为制造碰撞（同时发）时，退避后能自动恢复
- SF10 下 ACK 仍能按时回到（`ack_miss` 不因超时窗口过短而飙升）

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_ack_timeout_ms` / `g_retries`：ACK 参数（NVS 覆盖，`mac_set_params`）；**`g_ack_timeout_ms` 是下限，不是最终值**（P1-2，见下）
- `g_node_id`：本机地址（mac_start 从 NVS 读）
- `g_waiting_ack` / `g_ack_arrived` / `g_wait_seq`：ACK 等待状态
- `g_ack_ok` / `g_ack_miss` / `g_retry` / `g_tx`：统计计数

### mac_send 流程（同步阻塞）
1. `radio_airtime_ms(len)` → `duty_reserve()` 占空比限流（超限循环等 10ms，每轮喂狗）；未就绪时按满预算 100ms 记账
2. LBT `mac_lbt()`：CAD 最多 3 次（状态驱动轮询直到 CAD 完成，SF7≈8ms / SF10≈43ms 每次）；忙/错误/超时一律退避 5~20ms 重试（每次退避后喂狗），3 次仍不可用硬发
3. `radio_transmit(frame, len)`
4. 若 ack_req：算 `ack_to = max(g_ack_timeout_ms, radio_airtime_ms(16) + MAC_ACK_GUARD_MS)`，轮询等 ACK（5ms 步进，**轮询内每次 `watchdog_feed()`**——多次重传累计可能超过 5s WDT 超时），超时退避 100→300→700ms 重传，最多 g_retries 次

### ACK 超时为什么不能写死（P1-2）
- ACK 帧线上长度固定 16B（`MAC_ACK_FRAME_LEN` = 帧头 + CRC）。它的空中时间随 SF 变：**SF7 = 52ms，SF10 = 330ms**（见 `drivers/airtime.md`）
- 旧实现全部用 300ms：SF10 下收方还在发 ACK 就判超时 → 无谓重传、`ack_miss` 飙升，重传还挤占占空比
- 现在 `ack_to = max(NVS 值, ACK 帧 ToA + MAC_ACK_GUARD_MS=50ms)`：**NVS 值只放大不缩小**（默认 300ms 在 SF7 下行为与旧版一致，SF10 下自动升到 380ms）

### ACK 机制（跨模块，经 mesh_task）
- mesh_task 收 ack_req 数据帧 → `mac_send_ack(src_id, seq)`（echo seq，ack_req=0 防风暴）→ 发送走 **`radio_transmit_prio()`**（P0-2：射频单所有者的高优先请求队列）——原因见下：ACK 必须抢在排队的数据帧之前
- mesh_task 收 ACK 帧 → `mac_on_ack_rx()`（匹配 g_wait_seq 则置 g_ack_arrived）
- ACK 等待用轮询标志而非任务通知（mac_send 可能被 mesh_task 同步调用，避免通知绑定错误任务）

### mac_task（core0 prio 20）
任务开头 `watchdog_register_current()` 订阅任务看门狗，循环每轮 `watchdog_feed()`（P0-1）。
循环顶部先调 `adr_tick()`（ADR 周期切 SF，与收发同任务串行不冲突），再按 hi→med→lo 读 3 个优先级队列（hi/med 非阻塞，lo 5ms 超时），逐帧 mac_send

### 优先级队列
`rtos_tx_enqueue()` 按 qos 投 `q_mac_tx_hi/med/lo`（rtos.h 定义，rtos.cpp 建队列）——QOS2 控制指令真正优先
