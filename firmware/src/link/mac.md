# mac —— 信道接入

## 职责
决定「什么时候发、要不要回执、丢了多少次」。负责 LBT、ACK、重传、退避。

## 依赖
- `radio.h`（`radio_channelActivity` 做 CAD，`radio_transmit` 发包）
- `frame_codec.h`（解析 `ack_req` / `src_id` / `seq`）
- `adr.h`（mac_task 循环顶部调 `adr_tick` 切 SF）
- 被 `mesh`（转发）、`mav_router`（业务帧）调用

## 发送流程（`mac_send`）
1. **LBT**：`radio_channelActivity()`；忙 → 随机退避 5~20ms 再试，最多 3 次
2. 3 次仍忙 → **硬发**（控制指令不能无限憋，返回 `MAC_LBT_BUSY` 标记）
3. 发送 `radio_transmit`，失败返回 `MAC_TX_ERR`
4. 若 `ack_req`：等 ACK，超时按退避 **100→300→700ms** 重传，最多 `MAC_MAX_RETRIES` 次
5. 全失败返回 `MAC_ACK_TIMEOUT`，计 `ack_miss`；成功计 `ack_ok`

## ACK 约定
- 收到 `ack_req=1` 且 `dst_id` 匹配自己或广播 → 回 ACK 帧（`FRAME_ACK`，echo `src_id`+`seq`）
- ACK 帧本身 `ack_req=0`，不走重传（避免 ACK 风暴）

## 占空比联动
发送前先 `duty_reserve(airtime)`，超限则排队等（见 `duty` 模块）。

## 对外接口
见 `mac.h`。

## 验收（P1）
- 2 节点 500m 视距 0 丢包（`ack_miss=0`）
- 人为制造碰撞（同时发）时，退避后能自动恢复

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_ack_timeout_ms` / `g_retries`：ACK 参数（NVS 覆盖，mac_set_params）
- `g_node_id`：本机地址（mac_start 从 NVS 读）
- `g_waiting_ack` / `g_ack_arrived` / `g_wait_seq`：ACK 等待状态
- `g_ack_ok` / `g_ack_miss` / `g_retry` / `g_tx`：统计计数

### mac_send 流程（同步阻塞）
1. `duty_reserve()` 占空比限流（超限循环等 10ms）
2. LBT `mac_lbt()`：CAD 最多 3 次，忙退避 5~20ms，3 次仍忙硬发
3. `radio_transmit()`
4. 若 ack_req：轮询等 ACK（5ms 步进），超时退避 100→300→700ms 重传，最多 g_retries 次

### ACK 机制（跨模块，经 mesh_task）
- mesh_task 收 ack_req 数据帧 → `mac_send_ack(src_id, seq)`（echo seq，ack_req=0 防风暴）
- mesh_task 收 ACK 帧 → `mac_on_ack_rx()`（匹配 g_wait_seq 则置 g_ack_arrived）
- ACK 等待用轮询标志而非任务通知（mac_send 可能被 mesh_task 同步调用，避免通知绑定错误任务）

### mac_task（core0 prio 20）
循环顶部先调 `adr_tick()`（ADR 周期切 SF，与收发同任务串行不冲突），再按 hi→med→lo 读 3 个优先级队列（hi/med 非阻塞，lo 5ms 超时），逐帧 mac_send

### 优先级队列
`rtos_tx_enqueue()` 按 qos 投 `q_mac_tx_hi/med/lo`（rtos.h 定义，rtos.cpp 建队列）——QOS2 控制指令真正优先
