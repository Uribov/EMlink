# failsafe_policy —— 故障安全 + 电源状态机

## 职责

安全核心（架构 §8.2）：电源/接口状态机（防双供电）+ 链路看门狗。固件绝不越权替飞控做决定。

两条职责各自独立：电源状态机管「这块板当前该走哪条数据通路」，链路看门狗管「上行断了要通知谁」。


## 供电电路（已定案，架构 §8.2）

```
C 口 VBUS ────────────────→ 5V 引脚（S3 供电入口，无隔离，不能加二极管）
4Pin 5V ── SS34 ─────────→ 5V 引脚（单向，防倒灌回飞控/电池）
5V 引脚 ── PTC 0.5A ─────→ AMS1117 → 3.3V → E220
```

硬件上 SS34 和 PTC 各挡一路，固件状态机做第二道防线。


## 电源/接口状态机（硬约束：禁 Type-C 与 4Pin 同插）

判据 = 角色 + USB 是否枚举（不测 5V，测不出来源）。

```
上电读 role ──┬─ 未设 ──────→ 态A「仅 USB 配置」：USB=CLI，射频停
              ├─ role=AIR ──→ 态B「AIR 仅串口」：UART↔LoRa 数据通路
              └─ role=GCS ──→ 态C「GCS USB 跑 MAVLink」：USB=MAVLink

飞行中(态B) 若 USB 枚举(插上电脑) → 只弹 CLI，绝不切换数据通路（防数据流被 USB 抢走）
```

三态互斥，判据只看角色和 USB 枚举，不看电压。


## 链路看门狗（架构 §8.2 第 2 条）

- `failsafe_s`（默认 3s）无上行 → 置 `EV_LINK_LOST`：告警、停止无效转发
- 返航判定是飞控自己的事 —— 固件只保证 GCS HEARTBEAT 不误时（§mav_router）
- 恢复上行 → `EV_LINK_RESTORED`，自动重连

固件只上报链路状态，不做返航决策。


## 关键实现点

1. 状态机三态互斥，USB 枚举（插上电脑）在态 B 只触发 CLI，不改数据通路
2. 板级防反灌已实现：SS34（4Pin 侧）+ PTC 0.5A（AMS1117 前）；C 口侧不能加二极管（挡 4Pin 反向供电）。固件状态机做第二道防线

硬件挡一层、固件挡一层，两层都做到才允许两路同时插。


## 对外接口

见 `failsafe_policy.h`。

对外只有状态查询和事件入口两组函数，模块内部没有独立任务。


## 验收（P4）

- 飞行中插 USB → 数据通路不变，仅弹 CLI
- 掐断上行 3s 内飞控进返航；恢复后自动重连

两条都要上真板，且第二条需要飞控配合才能看出返航行为。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

安全核心：电源/接口状态机（防双供电）+ 链路看门狗。固件绝不越权替飞控做决定。


### 电源/接口状态机（三态，判据 = 角色 + USB 是否枚举）

| 态 | 条件 | 数据通路 |
|---|---|---|
| PWR_CFG_ONLY | role 未设 | 仅 USB=CLI，射频停 |
| PWR_AIR_UART | role=AIR | UART ↔ LoRa |
| PWR_GCS_USB | role=GCS | USB=MAVLink |

- `failsafe_enter()`：上电读 role 进对应态
- 飞行中（AIR 态）插 USB → 只弹 CLI，不切数据通路（防数据流被 USB 抢走）

状态只在 `failsafe_enter()` 里设置一次，运行期间不会因为插拔 USB 而切换。


### 链路看门狗

- `failsafe_on_event(ev)`：收到 EV_LINK_LOST 置 `g_link_lost=true`，EV_LINK_RESTORED 置 false
- 返航判定是飞控自己数心跳超时，固件只保证 GCS HEARTBEAT 不误时（见 mav_router）

本模块只维护一个布尔状态，事件由外部喂进来。


### 事件从哪来

- stats_task 每 1s：失联判定（EV_LINK_LOST/RESTORED）+ 读事件位调 `failsafe_on_event()`（failsafe 无独立任务）

失联判定挂在 stats_task 的 1s 周期上，所以实际的失联检出延迟是 1s 量级，不是毫秒级。


### 状态查询

`failsafe_power_state()` / `failsafe_link_lost()`，供 stats LED 显示

两个 getter 是只读的，LED 那边直接轮询即可。
