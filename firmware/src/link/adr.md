# adr —— 自适应速率（BEACON 锚定固定速率表）

## 职责
用「固定时间表 + BEACON 锚点」让全网节点分时切换扩频因子（SF）：
平时用 SF7 快速传数据，每周期留一段 SF10 慢速窗保链接（抗干扰、传得远，远处弱节点也能跟上）。

**不是真动态自适应**（不根据 RSSI/SNR 实时调），就是一张固定速率表，靠 GCS 的 BEACON 把全网节奏对齐。真自适应留后演进。

## 状态
**已实现**（P1-3 加固后）。所有角色（GCS/AIR/RELAY）默认参与，无需配置。

## 依赖
- `radio.h`（`radio_set_sf` 切 SF；`radio_airtime_ms` 算信标 ToA 做对齐补偿）
- `mesh.h`（GCS 发 `mesh_send_beacon`）
- `role.h`（判断本机是否 GCS）
- `frame_codec.h`（`FRAME_HDR_LEN`/`FRAME_CRC_LEN` 算信标长度）

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
- **非 GCS**：开机停在 SF10 等 BEACON。**直收**（`relay_id==0`）的 BEACON 那一刻 = 本机周期起点（`adr_on_beacon` 记 `epoch_ms`，并减掉信标 ToA + 排队延迟做补偿），之后按同一相位表切
- **多跳信标**（`relay_id!=0`）**不参与对齐**（中继的排队+空中时延无法补偿），只说明"系统在场"
- 全网靠 BEACON 对齐，SF 切换节奏一致，不会"我 SF7 你说 SF10"

## 兜底与自愈（P1-3）
| 情形 | 行为 |
|---|---|
| 曾对齐，权威信标超 `ADR_BEACON_LOST_MS`(15s) 没来 | 进**兼容模式**：固定 SF（`ADR_COMPAT_SF`=SF7），不再切窗；`失步` 计数 +1，串口告警 |
| 从未对齐，但收到过多跳信标（说明离锚点 > 1 跳）超过 15s | 同上（对齐在物理上做不到，别在 SF10 傻等） |
| 重新收到权威信标 | **自愈**：退出兼容模式，重新对齐周期 |
| 半周期内收到另一个锚点的信标 | `多锚点` 计数 +1（只告警，不做主从选举） |
| 本机是 GCS 时收到别人的信标 | 只计数，**不跟随**（否则两个 GCS 互拉相位） |

## 对外接口
见 `adr.h`：`adr_init / adr_on_beacon(src,relay) / adr_tick / adr_current_sf` + 诊断 `adr_beacon_count / adr_beacon_hop_count / adr_desync_count / adr_multi_anchor_count / adr_has_beacon / adr_compat_mode`。

## 验收
- GCS + 1 台 AIR：上电后 AIR 先停在 SF10 收到 GCS 第一个 BEACON，随后两者 SF 同步切换，5s 一轮
- 断掉 GCS，AIR 15s 后进兼容模式（`status` 的 `ADR:` 行显示 `对齐=降级`），**不再傻等**；GCS 回来后自动恢复 `对齐=是`
- 3 块板 A(直连)—B(2 跳)—C(3 跳)：C 进兼容模式而不是永远 SF10 收不到东西
- CLI `status` 能通过 `ADR:` 行看到 对齐/信标/多跳/失步/多锚点/SF

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_sf`：当前 SF（开机默认 SF10）
- `g_has_beacon`：是否已对齐到周期起点
- `g_epoch_ms`：本周期起点时刻（对齐后有效，含 ToA 补偿）
- `g_last_beacon_ms`：上次收到**权威** BEACON 时刻（失步判定用）
- `g_last_any_beacon_ms` / `g_first_any_beacon_ms`：任意信标（含多跳）的最近/首次时刻
- `g_anchor_src_id`：当前锚点 node_id（0=未知）
- `g_hop_count` / `g_beacon_count` / `g_desync` / `g_multi_anchor` / `g_compat`：计数与降级状态
- 以上全部 `volatile`：跨核（mac_task core0 写 + mesh_task core0 写 + core1 读 `adr_current_sf`）

### 状态机
```
GCS：
  每 5s → 切 SF10 → mesh_send_beacon() → 之后每 tick 按 phase_in_period 切 SF
非 GCS：
  g_compat           → 固定 SF7，不切窗（失去对齐基准）      ← P1-3
  未收过权威 BEACON   → 保持 SF10 等（15s 后若收到过多跳信标 → 进 g_compat）
  已对齐             → 每 tick 按 phase_in_period(now - epoch_ms) 切 SF
  权威信标超时 15s    → g_compat=true（+计数 +串口告警）
  再收到权威信标      → g_compat=false，重新对齐（自愈）
```

### 相位→SF 映射（sf_for_phase）
- `phase < ADR_SLOW_MS(1000)` → SF10（慢速窗）
- `phase >= ADR_PERIOD_MS - ADR_GUARD_MS(500)` → SF10（guard 窗）
- 其余 → SF7（快速窗）

### 挂接点
- `adr_tick()` 在 **mac_task 循环顶部**调用（每轮 ~5ms）：与射频收发**同任务串行**，切 SF 的 SPI 操作不会和 `radio_transmit`/`readPacket` 打架
  （`adr.h` 里"由 stats_task 1s 调一次"是旧注释，以此处为准；历史实现确实在 stats_task，已迁移）
- `adr_on_beacon(src_id, relay_id)` 在 **mesh_task** 收到 FRAME_BEACON 时调用（信标现在允许被中继转发一次，见 mesh.md）
- `adr_init()` 在 main.cpp `radio_start()` 后调用，先把射频停在 SF10

### 关键实现细节
1. **切 SF 的硬要求**：SX126x 芯片规定改调制参数（SF/BW）必须在 standby 态，不能在收/发中途写寄存器。故 `radio_set_sf` 顺序固定为 `standby() → setSpreadingFactor() → startReceive()`。
2. **开机先停 SF10**：否则 GCS 第一个 BEACON 是 SF10 发的，非 GCS 若停在 SF7 会永远收不到，直接断链。
3. **guard 窗**：BEACON 空中时间 ~200ms，收方 epoch 天然滞后于 GCS。若不加 0.5s guard，收方在周期末尾（SF7）会错过 GCS 在 t=0 发的下一个 BEACON，每轮都对不齐。
4. **BEACON 发送从 stats 移到 adr**：原 stats_task 每 1s 发 BEACON 的代码已删除，统一由 adr 5s 周期发，避免两个节奏打架。
5. **对齐补偿（P1-3）**：GCS 的时间戳是"把信标丢进发送队列"时刻，真正上空 = 入队 + 排队 + 空中时间；而节点侧基准是"收完"。故 `g_epoch_ms = 收完时刻 - (信标 ToA + ADR_BEACON_QUEUE_MS)`，把 ~330ms 的系统偏差还回去，不再侵占 500ms guard 窗。信标 ToA 直接由 `radio_airtime_ms(FRAME_HDR_LEN+FRAME_CRC_LEN)` 算（SF10 下 = 330ms）。
6. **多跳信标只做可见性（P1-3）**：信标 ttl 改为 `MESH_BEACON_TTL`(=2)，中继会转发一次；但 `relay_id!=0` 的信标**不进对齐逻辑**——中继的排队延迟与它的空中时间无法补偿，用它对齐只会更糟。
7. **降级为什么选 SF7（P1-3）**：兼容模式要挑一个"全网大多时间都开着"的速率——TDM 里 SF7 占 4/5 时间，所以未对齐节点用 SF7 能与 1 跳节点在大部分时间互通。**这是推断，参数待外场实测**。

### 遗留（后续演进）
- **多 GCS 主从选举**（当前只计数不选举：两个 GCS 同场会让非 GCS 的相位被交替重置）
- 真动态自适应：根据 `route` 里记录的邻居 RSSI/SNR，实时调 SF/BW/功率（方案留后）
- 功率自适应
- 对齐补偿的排队延迟是估算值（`ADR_BEACON_QUEUE_MS`），外场需按实测标定
