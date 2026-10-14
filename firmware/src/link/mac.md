# mac —— 信道接入

## 职责
决定「什么时候发、要不要回执、丢了多少次」。负责 LBT、ACK、重传、退避。

## 依赖
- `radio.h`（`radio_channelActivity` 做 CAD，`radio_transmit` 发包）
- `frame_codec.h`（解析 `ack_req` / `src_id` / `seq`）
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
