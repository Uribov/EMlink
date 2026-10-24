# mac —— 信道接入

## 职责

决定「什么时候发、要不要回执、丢了多少次」。负责发送准入（流量整形）、LBT、ACK、重传、退避。

所有出向的帧都从这里出去：发之前过两层限流和一次 LBT，要回执的等 ACK 并重传。


## 依赖

- `radio.h`（`radio_channelActivity` 做 CAD、`radio_transmit` 发包、`radio_airtime_ms` 算空中时间）
- `frame_codec.h`（解析 `ack_req` / `src_id` / `seq`；`frame_qos_of` 取 QoS）
- `duty.h`（占空比硬上限）
- `service/ratelimit.h`（按 QoS 的准入整形，P1-10）
- `adr.h`（mac_task 循环顶部调 `adr_tick` 切 SF、`air_beacon_tick` 发 AIR 低频信标）
- `service/watchdog.h`（mac_task 订阅任务看门狗；mac_send 等 ACK 期间喂狗）
- 被 `mesh`（转发）、`mav_router`（业务帧）调用

本模块不碰芯片、不碰 SPI，只调用 `radio_*` 和两个纯逻辑模块（duty、ratelimit）。


## 发送流程（`mac_send`）

1. 准入（P1-10）：`airtime = radio_airtime_ms(len)` → `ratelimit_allow(frame_qos_of(frame), airtime)`；超限直接丢，返回 `MAC_DROP_RATE`（不阻塞、不死等 —— 死等会把心跳/命令排在遥测后面，还会堵住 `adr_tick()`）
2. 占空比硬上限：`duty_reserve(airtime)`，超限循环等（每轮喂狗）；单帧超预算时 duty 自己会放行并计数（P1-2）
3. LBT（P0-3 修正语义）：`radio_channelActivity()` 只有返回 0（CAD 明确判定空闲）才放行；忙、错误、超时（状态未知）一律按「忙」处理，随机退避 5~20ms 再试，最多 3 次
4. 3 次仍忙则硬发（控制指令不能无限憋，返回 `MAC_LBT_BUSY` 标记）
5. 发送 `radio_transmit(frame, len)`（无超时参数，见 `radio.md`），失败返回 `MAC_TX_ERR`
6. 若 `ack_req`：等 ACK，超时 = max(NVS 配置值, ACK 帧空中时间 + 50ms)（P1-2），按退避 100→300→700ms 重传，最多 `MAC_MAX_RETRIES` 次
7. 全失败返回 `MAC_ACK_TIMEOUT`，计 `ack_miss`；成功计 `ack_ok`

七步里只有第 2 步和第 6 步会阻塞等待，其余都是先判断再决定发不发；需要回执的帧最多重传 `MAC_MAX_RETRIES` 次。


## ACK 约定

- 收到 `ack_req=1` 且 `dst_id` 匹配自己或广播 → 回 ACK 帧（`FRAME_ACK`，echo `src_id`+`seq`）
- ACK 帧本身 `ack_req=0`，不走重传（避免 ACK 风暴）

ACK 不参与重传，否则两个节点会互相刷 ACK。


## 两层限流（P1-2 + P1-10）

| 层 | 模块 | 性质 | 超限行为 |
|---|---|---|---|
| 准入整形 | `ratelimit` | 按 QoS 分类配额（控制流保底 10%），计价 = 空中毫秒 | 直接丢（`MAC_DROP_RATE`），无阻塞 |
| 占空比硬上限 | `duty` | 每节点每秒空中时间 ≤ 10%（法规） | 排队等（循环 `vTaskDelay(10ms)`，喂狗）；单帧超预算例外放行并计数 |

两层口径一致（都是空中毫秒/秒），准入只会更严；真正发出去的量始终受 duty 兜底。


## 对外接口

见 `mac.h`。`mac_result_t` 含 `MAC_DROP_RATE`（P1-10：被限流丢弃，调用方按丢帧处理即可，不要重试）。

返回值区分得比较细（OK / LBT_BUSY / TX_ERR / DROP_RATE / ACK_TIMEOUT），调用方按需处理即可，只有 `MAC_DROP_RATE` 明确不该重试。


## 验收（P1）

- 2 节点 500m 视距 0 丢包（`ack_miss=0`）
- 人为制造碰撞（同时发）时，退避后能自动恢复
- SF9 慢速窗下 ACK 仍能按时回到（`ack_miss` 不因超时窗口过短而飙升）
- 连续灌高频遥测时：控制类「限流丢弃」≈0、心跳间隔稳定（P1-10）

四条都要上真板；前两条测基本功能，后两条测限流和 ACK 超时在极端档位下是否还成立。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

所有出向帧的发送入口：准入整形 → 占空比限流 → LBT → 发送 → 等 ACK 重传，整条链在 `mac_task` 里同步跑完。


### static 变量

- `g_ack_timeout_ms` / `g_retries`：ACK 参数（NVS 覆盖，`mac_set_params`）；`g_ack_timeout_ms` 是下限，不是最终值（P1-2，见下）
- `g_node_id`：本机地址（mac_start 从 NVS 读）
- `g_waiting_ack` / `g_ack_arrived` / `g_wait_seq`：ACK 等待状态
- `g_ack_ok` / `g_ack_miss` / `g_retry` / `g_tx`：统计计数
- `g_tx_call` / `g_tx_done` / `g_tx_err_radio` / `g_tx_drop_rate` / `g_tx_duty_wait` / `g_tx_lbt_fail`：F22 发送分项诊断计数（`volatile`，只增不减、无锁），分别对应进入 `mac_send`、成功返回（`MAC_OK`/ACK 成功）、`radio_transmit` 非 0、令牌桶丢弃、`duty_reserve` 等待轮次、LBT 三次仍忙硬发；经 `mac_tx_*()` getter 暴露给 `cli status`

前四组是状态与统计，最后一组是 F22 加的发送诊断计数，排查「发了但对面收不到」时看这一组。


### mac_send 流程（同步阻塞）

1. `airtime = radio_airtime_ms(len)`（未就绪时按满预算 100ms 记账）
2. 准入：`ratelimit_allow(frame_qos_of(frame), airtime)` → false 则 `return MAC_DROP_RATE`（P1-10）
3. `duty_reserve(airtime)` 占空比限流（超限循环等 10ms，每轮喂狗）
4. LBT `mac_lbt()`：CAD 最多 3 次（状态驱动轮询直到 CAD 完成，SF7≈8ms / SF9≈23ms 每次）；忙/错误/超时一律退避 5~20ms 重试（每次退避后喂狗），3 次仍不可用硬发
5. `radio_transmit(frame, len)`
6. 若 ack_req：算 `ack_to = max(g_ack_timeout_ms, radio_airtime_ms(MAC_ACK_FRAME_LEN) + MAC_ACK_GUARD_MS)`，轮询等 ACK（5ms 步进，轮询内每次 `watchdog_feed()` —— 多次重传累计可能超过 5s WDT 超时），超时退避 100→300→700ms 重传，最多 g_retries 次

整个函数在 `mac_task` 里同步执行，从准入判断到发完（或等完 ACK）一气呵成，中途不切任务。


### ACK 超时的取值（P1-2）

- ACK 帧线上长度固定 `MAC_ACK_FRAME_LEN` = `FRAME_HDR_LEN` + `FRAME_CRC_LEN` = 17B（空 payload）。它的空中时间随 SF 变：SF7 = 51ms，SF9 = 165ms，SF10 = 330ms（见 `drivers/airtime.md`）
- 旧实现全部写死 300ms：高 SF 下收方还在发 ACK 就判超时，无谓重传、`ack_miss` 飙升，重传还挤占占空比
- 现在 `ack_to = max(NVS 值, ACK 帧 ToA + MAC_ACK_GUARD_MS=50ms)`：NVS 值只放大不缩小

规则就是取两者中较大的：配置值只是下限，实际超时跟着当前 SF 的 ACK 空中时间走。

本平台（慢速窗 SF9、快速窗 SF7）算下来 ACK ToA 加 guard 都不足 300ms，所以实际取值就是下限 300ms；只有 BW250 下用 SF10（ACK ToA 330ms）才会升到 380ms。


### 准入层的阻塞行为（P1-10）

`mac_send` 在 `mac_task` 里同步执行，而 `adr_tick()` 也在这个任务里 —— 一旦为等令牌/等占空比阻塞，ADR 的 SF 切换节拍一起被拖住，连锁后果是全网时间表失准。所以准入层只做「允许/丢弃」二选一，绝不等待。


### ACK 机制（跨模块，经 mesh_task）

- mesh_task 收 ack_req 数据帧 → `mac_send_ack(src_id, seq)`（echo seq，ack_req=0 防风暴）→ 发送走 `radio_transmit_prio()`（P0-2：射频单所有者的高优先请求队列）—— 原因见下：ACK 必须抢在排队的数据帧之前
- mesh_task 收 ACK 帧 → `mac_on_ack_rx()`（匹配 g_wait_seq 则置 g_ack_arrived）
- ACK 等待用轮询标志而非任务通知（mac_send 可能被 mesh_task 同步调用，避免通知绑定错误任务）

ACK 的收发两端都在 mesh_task 里，mac 只提供 `mac_send_ack` 和 `mac_on_ack_rx` 两个入口；等待用标志位而不是通知，是为了避免通知被绑到错误的调用者上。


### mac_task（core0 prio 20）

任务开头 `watchdog_register_current()` 订阅任务看门狗，循环每轮 `watchdog_feed()`（P0-1）。
循环顶部先调 `adr_tick()`（ADR 周期切 SF），紧跟 `air_beacon_tick()`（AIR 低频在场信标，独立于 ADR 开关），再按 hi→med→lo 读 3 个优先级队列（hi/med 非阻塞，lo 5ms 超时），逐帧 mac_send

`adr_tick()` / `air_beacon_tick()` 和所有发送在同一个任务里串行跑，这样切 SF 与信标发送不会和 SPI 收发撞上。


### 优先级队列

`rtos_tx_enqueue()` 按 qos 投 `q_mac_tx_hi/med/lo`（rtos.h 定义，rtos.cpp 建队列）—— QOS2 控制指令真正优先

三个队列只影响取帧顺序，不影响限流判断；限流在 `mac_send` 里统一做。


### 初始化

`mac_start()` 第一行调 `ratelimit_init()`：令牌桶是发送路径的准入层，必须先于任何发送初始化。
