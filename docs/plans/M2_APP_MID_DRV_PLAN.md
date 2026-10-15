# M2 方案：把"应用-中间-驱动"从理念变成可执行的约束

> 状态：**已拍板并实施**（D1 只引入检查、D2 角色状态下移、D4 三件、D5 只做批量烧录；落地记录见 `../reviews/FIX_LOG.md` 的 T-1/T-2/T-3/D2/FB 各行）。
> **本文件保留为决策记录**（当时的候选方案与拍板结论），不再更新；当前实现以各模块 `.md` + 总架构为准。
>
> **各决策的落地状态**（2026-10-15 补）：✅ 已实施　🔶 部分　⏸ 推后到上板后
>
> | 决策 | 内容 | 状态 |
> |---|---|---|
> | D1 | 只引入依赖检查（不进 CI） | ✅ 已实施：`tools/check_deps.py`（硬规则违规 0 处；`--strict` 留给将来） |
> | D2 | 角色状态下移到 `service/role_state` | ✅ 已实施（提交 `c46793c`）——中间层不再反向依赖 `app/` |
> | D3 | 应用层 5 处手写封装收敛到中间层出口 | ⏸ **推后到上板之后**（动的是发送主路径，必须在能当场回归时做；见 `ROADMAP.md` §3） |
> | D4 | 先做最疼的三件 | 🔶 协议版本 ✅（`bb977f5`）、计数按三层 ✅（`30f214a`）、**分级日志 ⏸ 推后**（要等真实调试经验告诉我们该看什么） |
> | D5 | 只做批量烧录 | ✅ 已实施：`tools/flash_batch.sh` + vendor `boot_app0.bin`（`c264ba6`） |
> 用户明确：架构理念 = **应用层 - 中间层 - 驱动层**，**现行目录（`app/ link/ net/ service/ drivers/`）就是按这个理念搭的**。
> 因此本方案 **不动目录、不动模块归属、不动启动方式**，只做三件事：**立契约 → 修越界 → 上严谨件**。

---

## 0. 三层契约（先定义清楚，再谈检查）

| 层 | 现在的目录 | 职责 | 允许依赖 | 禁止 |
|---|---|---|---|---|
| **应用层** | `app/`（+ 边界件 `service/mav_router`、`service/ota`） | 用户可见行为：角色/失效策略/CLI/统计/MAVLink 语义/维护 | 中间件的**对外接口**、基础支撑 | 不得自己拼帧/加密/入队；不得直接操作驱动寄存器级行为 |
| **中间层** | `link/` `net/` `service/`(通信部分) | 数传的全部机制：成帧/加密/一跳 ACK/占空比/LBT/ADR 时分/路由/去重/分片重组/限流 | 驱动层、基础支撑 | **不得 include `app/`**；不得理解 MAVLink 语义 |
| **驱动层** | `drivers/` `boards/` `radios/` | 硬件：射频、CAD、ToA、串口、USB、电源检测 | 基础支撑 | **不得 include 中间层/应用**（唯一例外：基础支撑，见下） |
| **基础支撑**（横切） | `rtos` `service/{nvs_cfg,watchdog}` + 未来的 `log`/`version` | 任务/队列/看门狗/参数/日志/版本 | 无（谁都不依赖） | 不得依赖任何层（不许反向） |
| 入口 | `main.cpp` | 组装与启动 | 全部（天然例外） | — |

> "基础支撑"是本次需要显式新增的一类：`watchdog`/`nvs_cfg` 被各层用（`drivers/radio.cpp` 就用 watchdog 喂狗），它们既不是中间件也不是驱动，归入横切层后，白名单就有依据了。

---

## 1. 机器体检结果（2026-10-15 实测，按上述契约判定）

| 检查项 | 结果 |
|---|---|
| 谁碰射频 | **只有 `link/mac.cpp`**（`:47` CAD、`:91` 发送、`:136` 高优先发送）→ 驱动层被中间层独占 ✅ 这条设计是对的 |
| 反向：谁 include 了 `app/` | `main.cpp`（入口，例外 ✅）、**`link/adr.cpp:10`**、**`service/ota.cpp:14`**（均为 `app/role.h`）→ **中间层倒挂 ❌** |
| 反向：驱动 include 上层 | **`drivers/radio.cpp:8`** `../service/watchdog.h` → 归入基础支撑后可放行（或改为注入），按契约需明确 |
| 越层动作：应用/中间件自己拼帧 | **5 处**：`service/mav_router.cpp:139-147`、`:273-281`、`net/mesh.cpp:202-213`、`:305-316`、`:336-344`（另有转发路径 `:477-484`）→ **「加密 → 成帧 → 入队」重复实现 ❌** |

---

## 2. 三件事

### A. 立契约 + 机器强制（`tools/check_deps.py` 进 CI）
- 白名单式规则：`app/ → {中间对外接口, 基础支撑}`；`link|net|service → {drivers, 基础支撑, 同层}`；`drivers → {基础支撑, 同层}`；`基础支撑 → {}`。
- 同时检查**越层动作**（比 include 更隐蔽的那类）：`app/` 内不许出现 `frame_encode` / `crypto_seal_frame` / `rtos_tx_enqueue` / `radio_transmit`（白名单按文件级豁免，逐条写明理由）。
- 输出：违规清单（文件:行 + 违反了哪条），CI 里非零退出即红。

### B. 修掉体检抓出的越界（三项，逐项可单独提交）
1. **中间层倒挂**：`adr.cpp`、`ota.cpp` 为了拿"角色"而 include `app/role.h`。
   - 方案 A（推荐）：把 `role` 的**枚举**下移到公共头（如 `rtos.h` 同级的 `role_types.h` 或 `service/types.h`），`app/role` 只保留策略逻辑；中间层只依赖枚举，不认识策略。
   - 方案 B：启动时注入（`adr_set_role_cb()`），中间层回调应用层取值 —— 更彻底但多一层间接。
2. **`drivers/radio.cpp → service/watchdog.h`**：把 `watchdog` 明确归入"基础支撑"（契约白名单放行），或改成注册式喂狗（radio 不认识 watchdog）。**推荐前者**（改动小、语义已正确）。
3. **5 处重复封装收敛**：应用/边界件不再自己 `seal+encode+enqueue`，统一调中间层的一个出口。
   - 方案 A（推荐）：复用已有的 `mesh_send()` 语义，给"已构造好的报文"加一条薄出口（`mesh_send_raw()` 或就近命名），调用点从 5 处降到 1 处。
   - 方案 B：新增中间层薄门面（`dlink_tx(payload, dst, qos, ack_req)`），`mesh_send` 内部也走它。
   - 收益：改协议（如 A-5 加 `enc_epoch`）只需改一处；加密不可能被漏（漏一处就是"某类报文不加密"）；中间层的计数能覆盖所有出网流量。

### C. 严谨件，按三层挂（不再自成一块）
| 项 | 按三层的落法 |
|---|---|
| **单测** | 驱动层：`airtime`（ToA 表 15 项断言入仓）、`frame_codec`（往返/越界/CRC）、`crypto`（nonce/AAD/重放窗/epoch 重同步）；中间层：`duty`（占空比决策）、`mac`（LBT/ACK 决策）、`mesh`（去重/TTL/路由老化/转发判定）、`frag`（重组键/槽满）、`ratelimit`（令牌曲线/回绕/超桶）；应用层：`mav_router`（路由决策 + 流限速表）、`nvs_cfg`（每参数边界）、`cli`（命令解析）。前提：`test/support/arduino_shim/`（最小 HAL 替身）+ `[env:native]` |
| **CI** | `build`（板级矩阵 + RAM/Flash 阈值）/ `test`（native）/ `lint`（clang-format，先做一次全量格式化提交）/ `docs`（模块 md 与代码一一对应、FIX_LOG 引用的提交号真实存在）/ **`deps`（本方案的越界检查）** / `release`（打 tag 产出 bin+sha256） |
| **版本号** | `FW_VERSION`（git describe）+ **协议版本**：帧格式版本随 **BEACON 广播**（1 字节：老固件只看到"payload 长 1"，能兼容；新固件收空 payload 按 0 处理），不一致 → 计数告警 + CLI 显示。**A-5 那种"改了帧头两端悄悄不兼容"正是它能拦住的**；`ver` 命令 + OTA 页面显示 |
| **日志** | 分级宏 + **按层 tag**（`[drv] [mid] [app]`），`dmesg [-l mid]` 按层过滤；ERROR/WARN 自动转 MAVLink STATUSTEXT（由应用层主动拉，不反向依赖）；缓冲 RAM 环形（默认 64×96B ≈ 6KB / +1.8%） |
| **计数与观测** | 每个计数归位到层：驱动（`rf_reinit`、收包队列丢）、中间（占空比超限/认证失败/重放/CRC/ACK 超时/去重/TTL/表淘汰/重组超时/限流）、应用（流限速/降频/队列丢弃）。`status` 按三层分行显示 |
| **RADIOSTATUS** | 1Hz 发向 GCS：`rxerrors` 按三层拆、`txbuf` = 发送队列水位、`remrssi` = 中间层记录的远端 RSSI、`noise` 取不到就填 0 并注明 |
| **出厂流程** | **按三层递进**：①驱动：空口 RSSI/噪声自检、串口/USB 通路 ②中间：一跳收发 + ACK 成功率 + 占空比合规 → 两跳投递 → 大报文分片重组 ③应用：MAVLink 心跳/参数/航点 + STATUSTEXT 与 RADIOSTATUS 在 QGC 可见。**一层不过不进下一层** |

---

## 3. 阶段（每阶段独立提交、独立验收，无板也能验）

| 阶段 | 内容 | 风险 |
|---|---|---|
| **P1** | 三层契约文档（含"基础支撑"分类）+ `check_deps.py` + `test/support/arduino_shim` + `[env:native]` + airtime 断言入仓 | 低（零行为改动） |
| **P2** | 修越界 ①②（角色枚举下移、watchdog 归类）+ 检查脚本转绿 | 低 |
| **P3** | 修越界 ③：5 处重复封装收敛为 1 处（**行为等价重构**） | 中 |
| **P4** | 单测铺开（三层各自成组） | 低 |
| **P5** | 版本号 + 协议版本（BEACON 携带）+ 日志分级/tag + 计数按层归集 + RADIOSTATUS | 低 |
| **P6** | CI 六 job + `tools/factory/*`（按三层递进，pty dry-run） | 低 |
| **P7** | 文档收口：架构文档把三层与数据流画清、MODULE_INDEX 补层归属列、dev 指南 | 低 |

---

## 4. 不做
- 不动目录名/模块归属（现行结构就是对的）
- 不引入 PX4 目录壳（`modules/ lib/ systemcmds/`、`.px4board`、`templates/`）
- 不引入 uORB / `.msg` 代码生成 / CMake+Kconfig
- 不把模块 `.md` 移出模块目录
- 不追求整机在 PC 上仿真（只做纯逻辑层 + HAL 替身）

---

## 5. 已拍板（2026-10-15，用户）

| 点 | 决策 | 落地方式 |
|---|---|---|
| **D1** 依赖检查 | **只引入检查**（不进 CI；越界项排队修，不阻塞） | `tools/check_deps.py`：默认只报告、退出码 0；`--strict` 留给未来 |
| **D2** 中间层拿"角色" | **角色枚举下移到公共头**，策略仍留 `app/role` | 消除 `link/adr.cpp`、`service/ota.cpp` 对 `app/` 的反向依赖 |
| **D3** 5 处重复封装 | **复用 `mesh_send` 加一条"已构造报文"的薄出口** | 应用/边界件不再自己 `seal+encode+enqueue` |
| **D4** 严谨件节奏 | **先做最疼的三件**：协议版本 + 计数按层 + 日志 | 其余（单测基建、CI、lint、RADIOSTATUS、产测）→ `ROADMAP.md` |
| **D5** 出厂流程 | **只做批量烧录**（`tools/flash_batch.sh`） | 三层递进产测脚本 → `ROADMAP.md` |
| 横切"基础支撑" | **成立**：`rtos` / `nvs_cfg` / `watchdog`（未来 `log`/`version`）归此类，各层可用 | 因此 `drivers/radio.cpp → service/watchdog.h` 不算违规，进白名单 |

**本批不做（已迁 `ROADMAP.md`）**：CI、主机侧单测基建、clang-format/lint 门禁、文档一致性检查脚本、RADIOSTATUS 上报、三层递进产测、启动脚本化、日志落 flash。

> 定位（用户强调）：本项目是**个人低成本开源项目**；产品级审查的目的是**减少上板调试时固件上的坑**，不是把工程化堆满。

