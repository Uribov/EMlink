# EMlink 任务书 B-2：ADR 兜底与自愈（对应审查日志 P1-3）

## 背景（为什么改）——四个真实缺陷

1. **信标 TTL=1 且从不转发**（`mesh.cpp` 的 BEACON 分支直接 `continue`）→ 只有 GCS **直连**的节点能收到信标；主场景"2+n 多跳"里，超出 1 跳的节点**永远停在 SF10 傻等**（`adr.cpp` 第 79-85 行），而全网其余节点大半时间在 SF7 窗 → 这些节点**既听不到也传不进去**，等于半瘫。
2. **无失步兜底**：收到一次信标后永久 `g_has_beacon=true`，之后靠本地晶振自由运行（±10~20ppm ≈ 每小时 40~70ms 漂移）。GCS 断电/换机后没人重新对齐，也没有任何告警。
3. **多 GCS 互相抢锚点**：收到任意信标就重置周期起点 → 两个 GCS 的信标交错到达 → 全网相位被反复重置。
4. **对齐基准有偏差**：GCS 的时间戳是在**把信标丢进发送队列**时打的，真正上空的时刻 = 入队 + 排队 + 空中时间；而节点侧的基准是"**收完**信标"。两侧天然错开 ≈ 信标 ToA(330ms) + 排队，而 guard 窗只有 500ms。

**本次修法（保守子集）**：信标可多跳但**只有直收（`relay_id==0`）的信标参与时间对齐**；失去对齐基准就**降级到固定速率的兼容模式并告警**（收到权威信标自动恢复）；多锚点只计数不选举；对齐点按信标 ToA 做补偿。**主从选举不做**（属多 GCS 产品形态，留给后续）。

## 允许改动的文件（其它一律不动）
1. `firmware/src/link/adr.h`
2. `firmware/src/link/adr.cpp`
3. `firmware/src/net/mesh.h`
4. `firmware/src/net/mesh.cpp`
5. `firmware/src/app/cli.cpp`

**不要动**：任何 `.md`、`nvs_cfg.*`、`radio.*`、`stats.*`、`main.cpp`；不要跑 pio/编译；不要 git commit/add。

## 1. adr.h：新增常量与接口
```c
// ---- P1-3 兜底 ----
#define ADR_BEACON_LOST_MS   15000  // 失去对齐基准这么久 → 进兼容模式（3 个周期）
#define ADR_BEACON_QUEUE_MS     50  // 估算的"入队→真正上空"延迟，对齐补偿用（外场可调）
#define ADR_COMPAT_SF        ADR_SF_FAST  // 兼容模式固定速率（TDM 里占比最大的那档）

// 收 BEACON 时调用：relay_id==0 表示本机直收（权威信标），否则是多跳转发来的
void adr_on_beacon(uint8_t src_id, uint8_t relay_id);

// 诊断
uint32_t adr_beacon_count(void);        // 权威信标数
uint32_t adr_beacon_hop_count(void);    // 多跳信标数（仅"系统在场"可见性）
uint32_t adr_desync_count(void);        // 进入兼容模式的次数
uint32_t adr_multi_anchor_count(void);  // 相位被别的锚点抢的次数
bool     adr_has_beacon(void);          // 是否处于对齐状态
bool     adr_compat_mode(void);         // 是否处于降级兼容模式
```
（`ADR_SF_FAST`/`ADR_SF_SLOW` 已有，勿改。）

## 2. adr.cpp

### (a) 新增 static
`g_beacon_count / g_hop_count / g_desync / g_multi_anchor / g_compat(false) / g_last_any_beacon_ms`；
`g_last_beacon_ms`（权威信标）保留并复用；新增 `g_anchor_src_id`（当前锚点 node_id，0=未知）。

### (b) `adr_init()`：把上面新增的变量清零。

### (c) 重写 `adr_on_beacon()`（**注意签名变成两个参数**）
```c
void adr_on_beacon(uint8_t src_id, uint8_t relay_id) {
    uint32_t now = millis();
    g_last_any_beacon_ms = now;

    if (relay_id != 0) {
        // 多跳信标：中继的排队+空中时延无法补偿，**不用于时间对齐**，只说明"系统在场"
        g_hop_count++;
        return;
    }

    // 权威信标：本机直收
    g_beacon_count++;

    if (role_current() == ROLE_GCS) {
        // GCS 自己就是锚点：不跟随别的锚点（否则两个 GCS 互相把相位拉来拉去）
        if (g_anchor_src_id != 0 && g_anchor_src_id != src_id) g_multi_anchor++;
        return;
    }

    if (g_has_beacon && g_anchor_src_id != src_id &&
        (uint32_t)(now - g_epoch_ms) < ADR_PERIOD_MS / 2) {
        g_multi_anchor++;   // 半周期内又冒出一个锚点 → 相位被抢（选举留待后续）
    }

    g_anchor_src_id = src_id;
    g_last_beacon_ms = now;
    g_compat = false;                    // 收到权威信标 → 退出兼容模式（自愈）

    // 对齐点补偿：GCS 的时间戳是"入队"时刻，本机基准是"收完"时刻，
    // 差 ≈ 信标空中时间 + 入队排队 → 减掉它，误差不再侵占 500ms guard 窗
    uint32_t toa = radio_airtime_ms(FRAME_HDR_LEN + FRAME_CRC_LEN);
    g_epoch_ms = now - (toa + ADR_BEACON_QUEUE_MS);
    g_has_beacon = true;
}
```
（`radio_airtime_ms` 来自 `radio.h`——`adr.cpp` 已 include；`FRAME_HDR_LEN`/`FRAME_CRC_LEN` 来自 `frame_codec.h`，经 `mesh.h` 已可见。若编译视角不确定，就在 `adr.cpp` 顶部显式 `#include "../link/frame_codec.h"`。）

### (d) `adr_tick()`：加失步判定 + 兼容模式
在函数里 `uint8_t role = role_current();` 之后、进入 GCS/非 GCS 分支之前，插入：
```c
    // P1-3 失步检测（只对非 GCS）：
    //  · 曾对齐过 → 权威信标超过 ADR_BEACON_LOST_MS 没来
    //  · 从未对齐 → 见过信标（含多跳，说明本机离锚点超过 1 跳）却始终没等到权威信标
    // 两种情况都说明"对齐基准没了/拿不到"：硬撑 TDM 只会丢包，降级到固定速率更好
    if (role != ROLE_GCS && !g_compat) {
        bool lost = false;
        if (g_has_beacon)              lost = (uint32_t)(now - g_last_beacon_ms) > ADR_BEACON_LOST_MS;
        else if (g_hop_count > 0)      lost = (uint32_t)(now - g_first_any_beacon_ms) > ADR_BEACON_LOST_MS;
        if (lost) {
            g_compat = true;
            g_desync++;
            Serial.printf("[adr] 失去对齐基准（%s）→ 进入兼容模式：固定 SF%u，不再切窗\n",
                          g_has_beacon ? "权威信标超时" : "离锚点超过 1 跳",
                          (uint8_t)ADR_COMPAT_SF);
        }
    }
```
（`g_first_any_beacon_ms` 用"第一次收到任意信标"的时刻，在 `adr_on_beacon` 里首次收到时（`g_last_any_beacon_ms==0`）记一次即可——请一并加上。）
在非 GCS 分支里、`if (!g_has_beacon)` **之前**插入：
```c
        if (g_compat) {
            // 兼容模式：固定速率，不切窗（已经失去对齐基准）
            if (g_sf != ADR_COMPAT_SF) { radio_set_sf(ADR_COMPAT_SF); g_sf = ADR_COMPAT_SF; }
            return;
        }
```
其它 TDM 逻辑**保持原样**（GCS 分支、相位切窗、guard 窗都不动）。

### (e) 文件末尾加四个计数访问器 + 两个状态查询
按上面 adr.h 的声明实现即可（只读，无需加锁）。

## 3. mesh.h / mesh.cpp

### mesh.h
```c
// BEACON 跳数上限（P1-3）：GCS 发 + 最多 1 跳转发。
// 为什么不是更大：信标在 SF10 下每跳 ≈0.4s，而慢速窗只有 1s，再远就跑出窗口了（外场可调）
#define MESH_BEACON_TTL  2
```

### mesh.cpp
- `mesh_send_beacon()`：把 `f.ttl = 1` 改成 `f.ttl = MESH_BEACON_TTL`（保持其余字段：空 payload、proto=CUSTOM）。
- `mesh_task` 的 BEACON 分支：**去掉那个 `continue`**，改成"处理完继续走通用转发判定"，并把信标信息传给 adr：
```c
        if (f.type == FRAME_BEACON) {
            route_learn(f.src_id, f.relay_id);
            route_update_rssi(f.src_id, item.rssi);
            adr_on_beacon(f.src_id, f.relay_id);   // P1-3：多跳信标只算可见性，不对齐
            // 不 continue：让信标继续走到下面的转发判定（dst=0、ttl>0 → 中继转发一次）
        } else if (...)   /* 其余分支保持原样 */
```
**注意**：
- 信标投递本机由 `adr_on_beacon` 承担，**不要**把它塞进 DATA/NACK/CFG 的投递段（那段的 `g_rx_cb` 不该收到信标）
- **不要**给信标加 ACK、不要加密、不要改 `frame_encode` 的任何字段语义
- 去重照旧生效（信标也要判重，否则环形拓扑会绕圈）

## 4. cli.cpp：status 增加一行
在现有「表容量」那行之后追加（status 由十行变十一行）：
```c
        snprintf(line, sizeof(line), "ADR: 对齐=%s 信标=%u 多跳=%u 失步=%u 多锚点=%u SF=%u\r\n",
                 adr_has_beacon() ? (adr_compat_mode() ? "降级" : "是") : "否",
                 (unsigned)adr_beacon_count(), (unsigned)adr_beacon_hop_count(),
                 (unsigned)adr_desync_count(), (unsigned)adr_multi_anchor_count(),
                 (unsigned)adr_current_sf());
        cli_print(line);
```
（`adr.h` 若未 include 请补。）

## 5. 自检（报告里给结论）
1. 为什么多跳信标**不能**参与时间对齐？（用"中继排队 + 空中时间无法补偿"解释）
2. 离锚点超过 1 跳的节点最终进入什么状态？它和 1 跳节点的帧还能不能互通？（说明兼容模式选 `ADR_SF_FAST` 的理由）
3. 收到权威信标后怎么自愈？`g_compat`/`g_has_beacon`/`g_anchor_src_id` 三个状态怎么变？
4. 两个 GCS 同场时，各自 `adr_tick` 的相位会不会被对方拉走？为什么？
5. 信标现在会被中继转发，环形拓扑里会不会无限绕圈？（说明去重表在这条路径上有没有生效）

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 5 条自检结论
3. 本次改动**不能**保证哪些性质（诚实列出，例如"多 GCS 主从选举没做""兼容模式的速率没实测"）
