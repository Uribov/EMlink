# adr —— 自适应速率（BEACON 锚定固定速率表）

## 职责

用「固定时间表 + BEACON 锚点」让全网节点分时切换扩频因子（SF）：
平时用 SF7 快速传数据，每周期留一段 SF9 慢速窗保链接（抗干扰、传得远，远处弱节点也能跟上）。

它不做真动态自适应（不根据 RSSI/SNR 实时调），就是一张固定速率表，靠 GCS 的 BEACON 把全网节奏对齐。真自适应留后演进。

换句话说：节奏是死的、对齐是活的 —— 全网什么时候用哪个 SF 由时间表定，各节点靠 BEACON 把自己的表和锚点对上。


## 状态

已实现（P1-3 加固后）。所有角色（GCS/AIR/RELAY）默认参与，无需配置。


## 依赖

- `radio.h`（`radio_set_sf` 切 SF；`radio_airtime_ms` 算信标 ToA 做对齐补偿）
- `mesh.h`（GCS 发 `mesh_send_beacon`）
- `role.h`（判断本机是否 GCS）
- `frame_codec.h`（`FRAME_HDR_LEN`/`FRAME_CRC_LEN` 算信标长度）

依赖集中在射频接口和 mesh 的信标发送，不反向依赖上层。


## 总开关 `adr_en`

- NVS 键：`adr_en`，取值 `1`（默认，开启）/ `0`（关闭）；只接受 0/1，其它值钳到 1。
- CLI：`set adr 0|1`（写入 NVS + 运行时开关；`save` 后重启确保射频按配置 SF 固定）。
- 关闭后的行为：
  - 固定使用 NVS 配置的 SF（`sf`，默认 7），不切速率；
  - `adr_tick()` 直接返回：不发 BEACON、不等 BEACON、不做相位协商；
  - `adr_on_beacon()` 忽略收到的信标，不会因为对端信标改 SF；
  - `adr_current_sf()` 返回配置里的 SF，而不是上次协商值；
  - 诊断计数（`信标/多跳/失步/多锚点`）保持原样，关闭时自然不增长。
- 用途：链路调试。先关掉 ADR、两端固定同一 SF，证明物理链路本身能通；再打开 ADR 调自适应。
- 边界：关闭 ADR 不等于关闭射频或信标帧类型，`mesh` 层该发什么还发什么；它只负责「不自动变速、不做时钟协商」。

关掉之后这台机器就退化成一个固定 SF 的普通节点，信标照收照发，只是不再跟着改自己的节奏。


## 速率表（方案定死，5s 保守周期）

| 相位（5s 周期内） | SF | 用途 |
|---|---|---|
| t=0~1s | SF9 | 慢速窗：GCS 发 BEACON（锚点）+ 保链接 |
| t=1~4.5s | SF7 | 快速窗：传数据 |
| t=4.5~5s | SF9 | guard 窗：提前回 SF9 等下一个 BEACON |

- 周期 5s（`ADR_PERIOD_MS`，用户拍板「保守些」）
- BEACON 固定 SF9（`ADR_SF_SLOW=9`），数据 SF7（`ADR_SF_FAST=7`）
  - 慢速窗取 9 的原因：本板芯片 LLCC68 在 125kHz 带宽下最高只支持 SF9（SF5-6-7-8-9），
    SF10@125kHz 是非法组合（亿佰特规格书 / Semtech Table 6-1 脚注 / Zephyr binding 三处一致，
    见 `docs/references/LLCC68_E220-900M22S.md` §2）。SF7 与 SF9 在 125/250/500kHz 下都合法。
- guard 0.5s（`ADR_GUARD_MS`）：BEACON 空中传播 + 处理约 165ms，收方周期起点天然比 GCS 晚，需提前回 SF9 否则每周期都错过 BEACON

三个时间窗加起来正好一个周期，慢速窗 1s、快速窗 3.5s、guard 0.5s。


## 时序

- GCS：自己就是锚点。每 5s 切 SF9 → 发 BEACON → 按相位切（0~1s SF9，1~4.5s SF7，4.5~5s SF9）
- 非 GCS：开机停在 SF9 等 BEACON。直收（`relay_id==0`）的 BEACON 那一刻 = 本机周期起点（`adr_on_beacon` 记 `epoch_ms`，并减掉信标 ToA + 排队延迟做补偿），之后按同一相位表切
- 多跳信标（`relay_id!=0`）不参与对齐（中继的排队+空中时延无法补偿），只说明「系统在场」
- 全网靠 BEACON 对齐，SF 切换节奏一致，不会「我 SF7 你说 SF9」

对齐的全部依据就是一个时刻值：直收 BEACON 的到达时刻减去传播与排队延迟。


## 兜底与自愈（P1-3）

| 情形 | 行为 |
|---|---|
| 曾对齐，权威信标超 `ADR_BEACON_LOST_MS`(15s) 没来 | 进兼容模式：固定 SF（`ADR_COMPAT_SF`=SF7），不再切窗；`失步` 计数 +1，串口告警 |
| 从未对齐，但收到过多跳信标（说明离锚点 > 1 跳）超过 15s | 同上（对齐在物理上做不到，别在 SF9 傻等） |
| 重新收到权威信标 | 自愈：退出兼容模式，重新对齐周期 |
| 半周期内收到另一个锚点的信标 | `多锚点` 计数 +1（只告警，不做主从选举） |
| 本机是 GCS 时收到别人的信标 | 只计数，不跟随（否则两个 GCS 互拉相位） |

三种情况都只降级不硬顶：拿不到对齐基准就固定到一个大多数节点都开着的速率，等信标回来再自愈。


## 对外接口

见 `adr.h`：`adr_init / adr_on_beacon(src,relay) / adr_tick / adr_current_sf / adr_set_enabled / adr_is_enabled` + 诊断 `adr_beacon_count / adr_beacon_hop_count / adr_desync_count / adr_multi_anchor_count / adr_has_beacon / adr_compat_mode`。

对外分两组：一组是控制与查询，一组是六个诊断计数。


## 验收

- GCS + 1 台 AIR：上电后 AIR 先停在 SF9 收到 GCS 第一个 BEACON，随后两者 SF 同步切换，5s 一轮
- 断掉 GCS，AIR 15s 后进兼容模式（`status` 的 `ADR:` 行显示 `对齐=降级`），不再傻等；GCS 回来后自动恢复 `对齐=是`
- 3 块板 A(直连)—B(2 跳)—C(3 跳)：C 进兼容模式而不是永远停在 SF9 收不到东西
- CLI `status` 能通过 `ADR:` 行看到 对齐/信标/多跳/失步/多锚点/SF

四条覆盖了正常同步、锚点丢失、多跳够不着、状态可观测。


---

## 当前实现架构（供人和子 agent 阅读）

### static 变量

- `g_sf`：当前 SF（开机默认 SF9）
- `g_has_beacon`：是否已对齐到周期起点
- `g_epoch_ms`：本周期起点时刻（对齐后有效，含 ToA 补偿）
- `g_last_beacon_ms`：上次收到权威 BEACON 时刻（失步判定用）
- `g_last_any_beacon_ms` / `g_first_any_beacon_ms`：任意信标（含多跳）的最近/首次时刻
- `g_anchor_src_id`：当前锚点 node_id（0=未知）
- `g_hop_count` / `g_beacon_count` / `g_desync` / `g_multi_anchor` / `g_compat`：计数与降级状态
- 以上全部 `volatile`：跨核（mac_task core0 写 + mesh_task core0 写 + core1 读 `adr_current_sf`）

前七个是状态，最后一行五个计数加一个降级标志；全部 volatile 是因为读写不在同一个核上。


### 状态机

```
GCS：
  每 5s → 切 SF9 → mesh_send_beacon() → 之后每 tick 按 phase_in_period 切 SF
非 GCS：
  g_compat           → 固定 SF7，不切窗（失去对齐基准）      ← P1-3
  未收过权威 BEACON   → 保持 SF9 等（15s 后若收到过多跳信标 → 进 g_compat）
  已对齐             → 每 tick 按 phase_in_period(now - epoch_ms) 切 SF
  权威信标超时 15s    → g_compat=true（+计数 +串口告警）
  再收到权威信标      → g_compat=false，重新对齐（自愈）
```

GCS 走独立分支，其余节点在「未对齐 / 已对齐 / 兼容」三个状态里流转。


### 相位→SF 映射（sf_for_phase）

- `phase < ADR_SLOW_MS(1000)` → SF9（慢速窗）
- `phase >= ADR_PERIOD_MS - ADR_GUARD_MS(500)` → SF9（guard 窗）
- 其余 → SF7（快速窗）

映射就是两个边界判断，周期首尾都是 SF9。


### 挂接点

- `adr_tick()` 在 mac_task 循环顶部调用（每轮 ~5ms）：与射频收发同任务串行，切 SF 的 SPI 操作不会和 `radio_transmit`/`readPacket` 打架
  （`adr.h` 里「由 stats_task 1s 调一次」是旧注释，以此处为准；历史实现确实在 stats_task，已迁移）
- `adr_on_beacon(src_id, relay_id)` 在 mesh_task 收到 FRAME_BEACON 时调用（信标现在允许被中继转发一次，见 mesh.md）
- `adr_init()` 在 main.cpp `radio_start()` 后调用，先把射频停在 SF9

三个挂接点分属两个任务：切 SF 在 mac_task，收信标在 mesh_task，初始化在 setup。


### 关键实现细节

1. 切 SF 的硬要求：SX126x 芯片规定改调制参数（SF/BW）必须在 standby 态，不能在收/发中途写寄存器。故 `radio_set_sf` 顺序固定为 `standby() → setSpreadingFactor() → startReceive()`。
2. 开机先停 SF9：否则 GCS 第一个 BEACON 是 SF9 发的，非 GCS 若停在 SF7 会永远收不到，直接断链。
3. guard 窗：BEACON 空中时间 ~165ms，收方 epoch 天然滞后于 GCS。若不加 0.5s guard，收方在周期末尾（SF7）会错过 GCS 在 t=0 发的下一个 BEACON，每轮都对不齐。
4. BEACON 发送从 stats 移到 adr：原 stats_task 每 1s 发 BEACON 的代码已删除，统一由 adr 5s 周期发，避免两个节奏打架。
5. 对齐补偿（P1-3）：GCS 的时间戳是「把信标丢进发送队列」时刻，真正上空 = 入队 + 排队 + 空中时间；而节点侧基准是「收完」。故 `g_epoch_ms = 收完时刻 - (信标 ToA + ADR_BEACON_QUEUE_MS)`，把 ~165ms 的系统偏差还回去，不再侵占 500ms guard 窗。信标 ToA 直接由 `radio_airtime_ms(FRAME_HDR_LEN+FRAME_CRC_LEN)` 算（SF9 下 = 165ms）。
6. 多跳信标只做可见性（P1-3）：信标 ttl 改为 `MESH_BEACON_TTL`(=2)，中继会转发一次；但 `relay_id!=0` 的信标不进对齐逻辑 —— 中继的排队延迟与它的空中时间无法补偿，用它对齐只会更糟。
7. 兼容模式选 SF7（P1-3）：兼容模式要挑一个「全网大多时间都开着」的速率 —— TDM 里 SF7 占 4/5 时间，所以未对齐节点用 SF7 能与 1 跳节点在大部分时间互通。这是推断，参数待外场实测。

前两条是芯片与时序的硬约束，中间三条是周期对齐的实现，最后两条是降级策略。


### 遗留（后续演进）

- 多 GCS 主从选举（当前只计数不选举：两个 GCS 同场会让非 GCS 的相位被交替重置）
- 真动态自适应：根据 `route` 里记录的邻居 RSSI/SNR，实时调 SF/BW/功率（方案留后）
- 功率自适应
- 对齐补偿的排队延迟是估算值（`ADR_BEACON_QUEUE_MS`），外场需按实测标定
