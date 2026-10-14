# adr —— 自适应速率（BEACON 锚定固定速率表）

## 职责
用「固定时间表 + BEACON 锚点」让全网节点分时切换扩频因子（SF）：
平时用 SF7 快速传数据，每周期留一段 SF10 慢速窗保链接（抗干扰、传得远，远处弱节点也能跟上）。

**不是真动态自适应**（不根据 RSSI/SNR 实时调），就是一张固定速率表，靠 GCS 的 BEACON 把全网节奏对齐。真自适应留后演进。

## 状态
**已实现**。所有角色（GCS/AIR/RELAY）默认参与，无需配置。

## 依赖
- `radio.h`（`radio_set_sf`，运行时切 SF）
- `mesh.h`（GCS 发 `mesh_send_beacon`）
- `role.h`（判断本机是否 GCS）

## 速率表（方案定死，5s 保守周期）
| 相位（5s 周期内） | SF | 用途 |
|---|---|---|
| t=0~1s | SF10 | 慢速窗：GCS 发 BEACON（锚点）+ 保链接 |
| t=1~4.5s | SF7 | 快速窗：传数据 |
| t=4.5~5s | SF10 | guard 窗：提前回 SF10 等下一个 BEACON |

- 周期 5s（`ADR_PERIOD_MS`，用户拍板"保守些"）
- BEACON 固定 SF10（`ADR_SF_SLOW=10`），数据 SF7（`ADR_SF_FAST=7`）
- guard 0.5s（`ADR_GUARD_MS`）：BEACON 空中传播 + 处理约 200ms，收方周期起点天然比 GCS 晚，需提前回 SF10 否则每周期都错过 BEACON

## 时序（关键）
- **GCS**：自己就是锚点。每 5s 切 SF10 → 发 BEACON → 按相位切（0~1s SF10，1~4.5s SF7，4.5~5s SF10）
- **非 GCS**：开机停在 SF10 傻等 BEACON。收到 BEACON 那一刻 = 本机周期起点（`adr_on_beacon` 记 `epoch_ms`），之后按同一相位表切
- 全网靠 BEACON 对齐，SF 切换节奏一致，不会"我 SF7 你说 SF10"

## 对外接口
见 `adr.h`：`adr_init / adr_on_beacon / adr_tick / adr_current_sf`。

## 验收
- GCS + 1 台 AIR：上电后 AIR 先停在 SF10 收到 GCS 第一个 BEACON，随后两者 SF 同步切换，5s 一轮
- 断掉 GCS，AIR 停留在 SF10 等待（不崩、不乱跳）
- CLI `stats` 能通过 rssi 反映 SF 切换后链路仍正常

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_sf`：当前 SF（开机默认 SF10）
- `g_has_beacon`：是否已对齐到周期起点
- `g_epoch_ms`：本周期起点时刻（对齐后有效）
- `g_last_beacon_ms`：GCS 上次发 BEACON 时刻

### 状态机
```
GCS：
  每 5s → 切 SF10 → mesh_send_beacon() → 之后每 tick 按 phase_in_period 切 SF
非 GCS：
  未收过 BEACON → 保持 SF10 傻等
  收过 → 每 tick 按 phase_in_period(now - epoch_ms) 切 SF
```

### 相位→SF 映射（sf_for_phase）
- `phase < ADR_SLOW_MS(1000)` → SF10（慢速窗）
- `phase >= ADR_PERIOD_MS - ADR_GUARD_MS(500)` → SF10（guard 窗）
- 其余 → SF7（快速窗）

### 挂接点
- `adr_tick()` 在 **mac_task 循环顶部**调用（每轮 ~5ms）：与射频收发**同任务串行**，切 SF 的 SPI 操作不会和 `radio_transmit`/`readPacket` 打架
- `adr_on_beacon()` 在 **mesh_task** 收到 FRAME_BEACON 时调用
- `adr_init()` 在 main.cpp `radio_start()` 后调用，先把射频停在 SF10

### 关键实现细节
1. **切 SF 的硬要求**：SX126x 芯片规定改调制参数（SF/BW）必须在 standby 态，不能在收/发中途写寄存器。故 `radio_set_sf` 顺序固定为 `standby() → setSpreadingFactor() → startReceive()`。
2. **开机先停 SF10**：否则 GCS 第一个 BEACON 是 SF10 发的，非 GCS 若停在 SF7 会永远收不到，直接断链。
3. **guard 窗**：BEACON 空中时间 ~200ms，收方 epoch 天然滞后于 GCS。若不加 0.5s guard，收方在周期末尾（SF7）会错过 GCS 在 t=0 发的下一个 BEACON，每轮都对不齐。
4. **BEACON 发送从 stats 移到 adr**：原 stats_task 每 1s 发 BEACON 的代码已删除，统一由 adr 5s 周期发，避免两个节奏打架。

### 遗留（后续演进）
- 真动态自适应：根据 `route` 里记录的邻居 RSSI/SNR，实时调 SF/BW/功率（方案留后）
- 功率自适应
