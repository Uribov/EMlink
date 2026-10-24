# INAV 航点桥接方案

## 简介

让地面站（QGC）能发航点给 iNav 飞控，飞机按航点自己飞。

难点在于 iNav 的 MAVLink 是「只出不进」：飞机能把遥测**发**给地面站，但地面站**没法**通过 MAVLink 把航点**传**进去。iNav 收航点只认 MSP 协议。

所以要在中间加一层转译：地面站用 MAVLink 说话，EMlink 把它翻译成 iNav 听得懂的 MSP。

这份方案只覆盖 **iNav**，Betaflight 先不管（它的自主飞行官方标注「仅供仿真、别真飞」）。


## 要解决的问题

```
地面站（QGC）             EMlink                 iNav 飞控
    │                      │                      │
    │  发航点（MAVLink）    │    ??? MSP            │
    └──────────────────────┼──────────────────────>
                           │
    <──────────────────────┼──────────────────────  遥测（MAVLink，本来就通）
```

反方向（飞机 → 地面站）**不用做**，iNav 自己就会输出 MAVLink 遥测。


## 技术事实（含出处）

### iNav 的能力边界

| 事实 | 出处 |
|---|---|
| iNav 的 MAVLink 实现只支持 push telemetry，即任务**监控**，**不能任务规划** | INAV Missions 文档 |
| 航点上传要用 MSP（`MSP_WP` / `MSP_SET_WP`） | INAV MSP Navigation Messages |
| 航点数量上限：F1 板 30 个，F3 及更好 60 个 | INAV Navigation 文档 |
| NAV WP 模式「用一个开关激活，和别的模式一样」 | INAV Navigation Modes 文档 |
| 导航模式前置条件：ACC/MAG 校准、有效 3D GPS fix、有效高度源、已 arm | 同上 |

### MSP 航点报文（21 字节）

| 字段 | 类型 | 说明 |
|---|---|---|
| `wp_no` | u8 | 航点序号 |
| `action` | u8 | 动作类型 |
| `lat` | i32 | 纬度 × 1e7 |
| `lon` | i32 | 经度 × 1e7 |
| `alt` | i32 | 高度（厘米，相对 home） |
| `p1` / `p2` / `p3` | i16 × 3 | 随 action 变 |
| `flag` | u8 | `0xa5` = 最后一个，其余 0 |

动作类型（先做前三个就够用）：

| 值 | 名称 | p1 | 说明 |
|---|---|---|---|
| 1 | WAYPOINT | 速度 cm/s | 普通航点 |
| 3 | POSHOLD_TIME | 悬停秒数 | 悬停等待 |
| 4 | RTH | 降落标志 | 返航 |
| 8 | LAND | 速度 cm/s | 降落 |

`MSP_WP`(118) 是读，`MSP_SET_WP`(209) 是写。

### MAVLink MISSION 协议（握手式）

上传不是一发了事，是逐条握手：

```
地面站 → MISSION_COUNT(count)          一共几个点
飞控   → MISSION_REQUEST_INT(seq=0)    我要第 0 个
地面站 → MISSION_ITEM_INT(seq=0)       给你
          ⋮  循环到最后一个
飞控   → MISSION_ACK(ACCEPTED)         收齐
```

超时 1.5s、最多重试 5 次。下载是反过来的（`MISSION_REQUEST_LIST` → `MISSION_COUNT` → 循环）。

坐标换算很干净：`MISSION_ITEM_INT` 的 `x`=纬度×1e7、`y`=经度×1e7，和 MSP 完全一致；`z` 是米（float），MSP 是厘米（int），差 100 倍。

`MISSION_ITEM_INT` 是 46 字节，远小于本项目 `FRAME_MAX_PLAIN`(175)，**不需要分片**。

出处：MAVLink 官方 Mission Protocol 页面。


## 候选方案

### 方案 A：AIR 端转译（EMlink 冒充飞控）

iNav 那侧的 EMlink 假装自己是一台飞控，对地面站的 MISSION 握手照常应答，同时把每条航点转成 MSP 写进真飞控。

```
QGC ──MAVLink 握手──> EMlink(AIR) ──MSP_SET_WP──> iNav
```

- 地面站完全无感，以为在跟真飞控说话，操作方式不变
- 转译在飞机侧本地完成，不占空口带宽（航点不用一条条过 LoRa）
- 代价：AIR 端要维护一个握手状态机（等哪条、发哪条、超时重发）
- 落点：新模块 `firmware/src/service/mission_bridge.cpp`

### 方案 B：地面端转译

电脑侧（EM Studio 之类）先转好协议，再把结果送进 mesh。

```
QGC ──> 电脑转换 ──> EMlink mesh ──> iNav
```

- 不用改固件（如果只改上位机）
- 代价：航点要一条条过 LoRa 空口，慢；且电脑必须一直开着
- 代价：EM Studio 是调试工具，不是地面站，定位会乱

按优先级「稳定性高 > 复杂度小 > 可拓展强 > 改错容易」，**推荐 A**：空口不占带宽、地面站无感、状态机是局部复杂度，不会扩散。


## 关于模式启动

iNav 飞航点要靠遥控器拨到 NAV WP 档启动 —— 这一步**不做进转译层**。

理由：用户手上有遥控器，且多机场景下用同一个遥控器（关掉 Model Match）可以一次给所有飞机 arm 并拨档。把「启动任务」也做进 EMlink，等于让数传掌握了飞机能不能起飞，风险不成比例。

**但有一个前提要你确认**：多机同时飞时，所有飞机的 NAV WP 模式是同一个遥控通道控制的 —— 也就是说它们会**同时开始飞各自的航点**。如果不是你要的（比如想陆续起飞），那方案要调整。


## 决策点

1. **转译放哪端？** 推荐 AIR 端（方案 A）。选地面端（方案 B）的话，航点要过空口，速度会慢很多。
2. **先支持哪几种动作？** 建议只做 `WAYPOINT`(1) + `RTH`(4) + `LAND`(8)，`POSHOLD_TIME`(3) 等其他类型等验证过再加。
3. **要不要做反向（读回航点）？** iNav → QGC 的航点下载（MISSION_REQUEST_LIST 那一套）。不做的话地面站上看不见飞机里已有的航点。
4. **高度基准怎么定？** MSP 的高度是「相对 home 的厘米」，MAVLink 有相对/绝对两种 frame。建议先只支持相对高度，遇到绝对高度的航点直接报错拒绝，不猜。
5. **多机同时启动可以接受吗？** 见上一节，所有飞机同时开始飞。要能单独启动就得用 Model Match 分配不同遥控器模型。
6. **要不要顺带做 arm 转译？** 地面站发 `MAV_CMD_COMPONENT_ARM_DISARM`(400) → 转成什么？iNav 侧没有干净的等价 MSP 命令（要么靠遥控通道，要么用 `MSP_SET_RAW_RC` 注入，后者要求飞控固件编译时开 `USE_MSP_RC_OVERRIDE`，且 EMlink 断线即失控）。建议**不做**，arm 留给遥控器。

---

本文件是方案稿，拍板前不提交。旧稿被否直接删。
