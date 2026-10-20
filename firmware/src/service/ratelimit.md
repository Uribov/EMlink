# ratelimit —— 按 QoS 的令牌桶流量整形

## 职责

按 QoS 分类分配「每秒能占用多少空口时间」，控制流有保底份额，让心跳/命令永远不会被遥测流挤下线。
计价单位 = 空中毫秒（airtime，与 duty 同口径），不是包数 —— 一个 200B 的包在 SF7 就要占 318ms 空口，按包数算等于没算。

它是发送路径上的第一道闸，比 duty 更早介入：不排队，直接按类丢弃。


## 状态

已实现（P1-10）。默认参数即可用，份额常量编译期可调。


## 依赖

- `frame_codec.h`（`QOS2` 常量 + `frame_qos_of()` 取帧内 QoS）
- 被 `mac`（`mac_send` 准入）使用
- 与 `duty` 的分工：duty 是硬上限（法规 10% 占空比，超了会死等/放行计数），ratelimit 是准入整形（提前按类丢弃）。两层口径一致（都是空中毫秒/秒），ratelimit 只会更严，不会让总量超过 duty

两层限流的分工是「谁先谁后、谁是硬闸」的问题，不是重复计数。


## 设计取舍

| 问题（审查日志 P1-10） | 本模块的做法 |
|---|---|
| 地面站拉 10~50Hz 遥测，把控制流挤下空口 | 控制类（QOS2）独立桶，保底 10% 空口时间；用户类（QOS0/QOS1）拿剩下 90% |
| `mac_send` 占空比超限时原地死等 → 心跳被排队、`adr_tick()` 被堵住 | 准入即丢、不阻塞：超限直接返回 false，调用方丢帧并计数（绝不 `vTaskDelay` 等令牌） |
| 队列满静默丢、无计数 | 丢弃计数 + CLI `status` 可见 |

两个问题对应两个设计选择：分桶解决「谁被挤」，不阻塞解决「挤了之后怎么办」。


## 参数（编译期常量，外场按实测调）

| 常量 | 值 | 含义 |
|---|---|---|
| `RL_CTRL_RESERVE_PCT` | 10 | 控制流保底份额（% 的每秒空口时间） |
| `RL_CTRL_BURST_MS` | 30 | 控制桶突发上限（ms） |
| `RL_USER_BURST_MS` | 100 | 用户桶突发上限（ms，≈每秒配额） |
| `RL_WINDOW_MS` | 1000 | 份额基准周期（换算用） |

控制桶的突发上限只有 30ms，比用户桶的 100ms 小得多，这是刻意的。


## 对外接口

见 `ratelimit.h`：`ratelimit_init / ratelimit_allow(qos, airtime_ms) / ratelimit_drop_count / ratelimit_ctrl_drop_count / ratelimit_oversize_count`。

对外一个判断函数加三个计数，桶的状态全部藏在模块内。


## 验收

- 连续灌高频遥测时：用户类「限流丢弃」增长，控制类接近 0，心跳间隔稳定（不出现 1s+ 抖动）
- SF10 大分片（1.64s > 桶容量）不会被永久丢光（走「超桶放行」计数）
- CLI `status` 的「占空比」行能看到三类计数

第一条是主要目标，后两条是边界与可观测性。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

按 QoS 分桶分配每秒的空口时间，控制流有保底份额，永远不被遥测流挤下线。计价用空中毫秒，不是包数。


### static 变量

- `g_ctrl_tokens` / `g_user_tokens`：两桶当前令牌（单位 = 空中毫秒）
- `g_ctrl_frac` / `g_user_frac`：未满 1 令牌的余量（0.01 令牌为单位，防整数除法丢余量）
- `g_last_ms`：上次补充时刻
- `g_drop` / `g_ctrl_drop` / `g_oversize`：计数

两个桶各自带一份余量，是为了解决高频调用下的整数除法丢精度。


### 关键实现细节

1. 补充（`rl_refill`）：`dt = now - g_last_ms`；控制桶 `g_ctrl_frac += dt*10`、用户桶 `g_user_frac += dt*90`（单位百分之一令牌），`/=100` 进桶、`%=100` 留余量。
   不能写成 `dt * PCT / 100` 直接除 —— `mac_send` 高频调用（dt<10ms）时整数除法会把余量全丢，控制桶永远补不回来（而那正是限流最该起作用的时刻）。
2. 桶上限：补充后各自截断到 `*_BURST_MS`（令牌桶语义：允许一次性突发，长期速率受限）
3. 准入（`ratelimit_allow`）：`QOS2` 走控制桶，其余走用户桶；单帧 airtime > 桶容量时直接放行并计 `g_oversize`（否则 SF10 大分片永远攒不够令牌 → 大包彻底传不动；与 duty 的 P1-2 例外同构）
4. 不阻塞：返回 false 即丢，`mac_send` 把它转成 `MAC_DROP_RATE` 返回（上层不重试、不死等）
5. 无线程安全措施是有意的：`mac_send` 只在 `mac_task` 单线程上下文调用，桶与计数无需加锁；访问器只读（与其它统计接口一致，允许近似读）
6. 初始化：`mac_start()` 第一行调 `ratelimit_init()`（令牌桶是发送路径的准入层，必须先于任何发送初始化）

第 1 条和第 5 条是这套实现里最容易改错的：余量的记法不能简化，不加锁是刻意的。


### 调用链

```
mac_send()
  → airtime = radio_airtime_ms(len)              （未就绪按满预算记账）
  → ratelimit_allow(frame_qos_of(frame), airtime)
       false → return MAC_DROP_RATE              （已计数，直接丢，不死等）
       true  → duty_reserve(airtime)（硬上限）→ LBT/CAD → radio_transmit
```
