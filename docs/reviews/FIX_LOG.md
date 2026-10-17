# EMlink 修复对照表（FIX LOG）

> 审查基线：`CODE_REVIEW_2026-10-14.md`（原样保留，不修改；里面多数问题**已修**，现状以本文件为准）。工作流与目录说明见 `README.md`；任务书**不进仓库**（源在 `~/EMlink-briefs/`，完成即归档到其 `done/`，规矩见 `briefs/README.md`）—— 长期记录只有本文件的对照表。

## M0 —— 上板前阻断项（已完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **P0-1** 看门狗接错人 | `d475fd5` | 不再订阅 `loopTask`（Arduino 的 loopTask 只有 `enableLoopWDT()` 后才喂狗，本工程没调 → 上板必现 5s 复位循环）；rf/mac/mesh/mavlink/stats 各自 `watchdog_register_current()` + 循环内喂狗；rf 等 DIO1 通知带 1s 超时 | 编译过；逐个订阅任务的循环里都有 `watchdog_feed()` 核对 | 真"72h 无意外复位"要上板才成立 |
| **P0-4** 配置零校验 | `b01ff96` | `nvs_cfg` 集中校验（CLI/远程/网页共用同一入口；freq 862–930、sf 7–11、power -9~22、strtol/strtod 严格尾校验）；`node_id` 首次上电 MAC 派生并落盘 + 撞号计数；CLI/网页新增 `factory` 恢复出厂；`radio_is_ready` + rf_task 每 10s 重试 + 失败整灯红 | 编译过；校验范围与文档/架构 §9 一致性核对 | — |
| **P0-3** CAD 结果读早了 | `565af4c` | LBT 改状态驱动轮询（原实现启动后立刻读结果 → 永远判空闲，LBT 形同虚设）；超时按 SF/BW 推导（5 符号+3ms，SF7≈8ms / SF10≈43ms）；忙/错/超时一律按忙退避；等待期间补喂狗 | 编译过；超时公式手算 | 空口实测转发率 |
| **P0-2** 射频非单所有者 | `4355b14` | 所有 SPI/芯片操作收敛到 `rf_task`（`q_radio_hi/lo` 请求队列 + 任务通知回结果）；新增 `radio_transmit_prio` 让 ACK 走高优先队列 | 编译过；上层调用方式零改动核对 | 上板并发压测 |

## M1 "A 档" —— 可脱离硬件验证的交付级修复（已完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **A1 / P1-2** 时延差 5~28× | `b658674` | 新增 `drivers/airtime`（Semtech ToA 整数实现）；占空比与 ACK 超时全按它推导；`duty_reserve` 单帧超预算放行计数；`radio_transmit` 去掉被静默忽略的第三参 | **`airtime.cpp` 抽到主机侧编译跑 15 项断言**，与审查日志 ToA 表逐值吻合；编译过 | 空口实测占空比/ACK 成功率 |
| **A2 / P1-7** sysid 未接线 | `1cd1471` | 本机 sysid 由 NVS 决定（0=角色默认）；同 sysid 撞号检测（排除自己中继回来的帧） | 编译过；`role_apply → nvs_cfg_load → mav_router_init` 链路核对 | 多机同场实测 |
| **A3 / P1-8** 认证失败连转发一起吞 | `ef96ae2` | 解密失败只跳过**本机投递**，不再 `continue` 吞掉转发；`mesh_drop_auth_count()` | 编译过；逐处排查其它 `continue` 是否会连带吞转发（结论：无） | ACK 先于认证仍是 P1-1 范围 |
| **A4a / P1-9** 表满静默退化 | `8d52cc3` | 去重表 LRU 淘汰；route TTL 老化（过期 `route_next_hop` 返回 0 → 回退广播）；addr_map TTL；三表计数进 CLI | 编译过；有符号差值比较（回绕安全）核对；临界区闭合核对 | TTL 参数待外场调 |
| **A4b / P1-9** 重组键漏 `src_id` | `5f717b3` | 重组键加 `src_id`；4 槽满淘汰最早者并计数 | 编译过；键与槽字段一致性核对 | — |
| **A5a / P1-4** 帧头加世代号 | `f028795` | 帧头 14→15 字节，新增 `enc_epoch`；`nvs_cfg_next_enc_epoch()`（独立 NVS 键，未动 `cfg_t`） | 编译过；编解码偏移对称核对（10/11/15） | **协议不兼容**：两端必须同版本 |
| **A5b / P1-4** AEAD 加固 | `5cf0abe` | nonce 加 epoch（原实现跨重启必复用）；AAD 7B=逐跳不变帧头；收端 8 源×64 位重放窗；seal 先盖 epoch | 编译过；**重放窗位运算逐分支手算**；AAD 字段集与转发路径核对 | epoch 仅 1 字节（256 次开机后可能重现） |
| **A5c / P1-4** 重放窗"重启饿死" | `5f347a3` | 收端窗口按 `enc_epoch` 重同步：同源 epoch 变 ⇒ 对方重启 ⇒ 重置窗口并接受；同 epoch 内仍严格 | 编译过；自检 4 题（含"严格窗口有没有被削弱"） | 持有旧世代真密文者可触发一次窗口重置（用户拍板方案 A） |
| **A6 / P1-5** OTA 不确认固件 | `0c59ac6` | **实测 sdkconfig 确认平台启用了 APP_ROLLBACK** → 原实现刷完重启会被悄悄回滚；新增 `ota_boot_confirm_tick()`（10s + 射频就绪才确认）；维护模式空闲 5min/硬上限 30min 自动退出 | 编译过；回滚开关**从 sdkconfig 实证**；自检 4 题 | 网页/OTA 仍无鉴权（产品决策） |
| **A7 / P1-12** 死代码/未接线 | `4783e31` | 删 `q_frag`/`mac_should_ack`/`ota_handle`/`duty_set_max`；补 `route/addr_map/frag_init()` 调用；接线 `EV_CFG_CHANGED` 与 `name`；`mav_router_feed` 改调 `handle_msg`（消重） | 编译过；`mav_router_handle_msg` 与被替换调用逐行比对（等价）；队列删除残留检查 | P1-12 第 1/3/4 条未做 |

## M1 "B 档" —— 改法清楚、结论只能上板（本轮完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **B-1 / P1-11** OCP 过流保护 | `ff56082` | `begin()` 内部把 OCP 设成 60mA，而 +22dBm 需 120~140mA；功率 >14dBm 时在 `begin()` **之后**调 `setCurrentLimit(140.0f)`（init + 重试两处） | 编译过；`setCurrentLimit` 签名与 `begin()` 里的 60.0 **从 RadioLib 源码实证** | **驱动选型保守未动**（默认仍 SX1268）；A/B 步骤写进 `todo.md` |
| **B-2 / P1-3** ADR 兜底与自愈 | `e1de762` | 信标 ttl 1→`MESH_BEACON_TTL`(2) 可中继一次；**只有直收信标参与对齐**；失步 15s（权威超时／只见多跳信标）→ 降级固定速率兼容模式 + 告警，收权威信标即自愈；多锚点只计数；对齐点按信标 ToA 补偿 | 编译过；信标最大跳数按 `ttl` + `mesh_ingress` 递减逐跳手推（确认 1 跳转发）；环形不绕圈=去重表+TTL 双保险核对 | 多 GCS 主从选举未做；兼容模式速率 SF7 是推断；补偿里的排队延迟是估算 |
| **B-3a/B-3b / P1-10** 令牌桶流量整形 | `992d646` | 新增 `service/ratelimit`：按 QoS 分桶（控制类保底 10%、用户类 90%，计价=空中毫秒）；**准入即丢不阻塞**（`MAC_DROP_RATE`）；单帧超桶放行；补充令牌改为**余量累加**；`mac_start` 调 `ratelimit_init` | 编译过；令牌数学逐项手算（含 dt=5ms 修复前后对比：0 → 正常）；与 duty 的层序/口径核对 | 份额参数（10%/30ms/100ms）待外场测；QOS1 与 QOS0 未细分 |
| **B-3c / P1-10** 各层丢弃计数 + 链路质量 | `b95da6e` | 4 个丢弃点加计数（发送队列/收包队列/请求队列/UART/USB——原来全静默丢）；`link_quality_pct` 由恒 0 改为 ACK 成功率；CLI 新增「丢包」行 | 编译过；`rtos_tx_enqueue` 改写后与三个 `return` 分支等价比对；各计数写入者的任务上下文核对（单写多读） | 上报到 MAVLink RADIO_STATUS 属 P2 |
| **B-3d / P1-10** MAVLink 流限速 | `a628568` | `mav_router` 按消息 ID 降频（SYS_STATUS 1s；GPS/ATTITUDE/位置/VFR_HUD 500ms；IMU/RC 200ms）；HEARTBEAT/COMMAND/PARAM/MISSION 永远放行；表外 msgid 一律放行；计数进 CLI「遥测降频」 | 编译过；限速表语义与"未知不擅自丢"策略核对；单线程上下文核对 | 阈值待实测；未区分上下行来源 |

## 上板 bring-up（2026-10-17，首块 ESP32-S3 开发板）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **S3** 4MB 板刷完启动崩溃 | 未提交 | `platformio.ini` 只设了 `board_build.flash_size`，而 esptool 的 `--flash_size` 取的是 board 的 `upload.flash_size`（`esp32-s3-devkitc-1` 默认 8MB）→ 镜像头声明 8MB、实板 4MB → `assert failed: do_core_init` 重启循环。补 `board_upload.flash_size = 4MB` + `board_upload.maximum_size = 4194304` | 上板：串口不再报 `Detected size(4096k) ... (8192k)`，正常启动到 CLI | 换 8MB 板需另建 env |
| **S4** 无射频模块时启动死锁 | 未提交 | ①`radio_call()` 等待加 3s 超时（原 `portMAX_DELAY`）；②请求队列改**值拷贝**（原指针 + 超时 = use-after-free，实测 `LoadProhibited`，`addr2line` 定位到 `rf_task_func`）；③从未成功过即判定"无模块"→`g_absent`，一次都不自动重试、所有射频操作立即失败；④只有 `g_ever_ready` 才 `watchdog_rf_fault()`；⑤启动日志与 CLI 明确报"未检测到射频模块（err=…）" | 上板（**无 E220**）：启动到 `===== 就绪 =====`；`status` 显示 `模块=未检测到（err=-2）`、`wdt复位=0`、观察期零重启；`wifi` 起来 AP `EMlink-187 @ 192.168.4.1` | `begin()` 内部死等未根除（靠"无模块不重试"绕开）；超时后请求可能迟到执行；WiFi 页面功能待手机实测 |

## 本轮新发现（审查日志之外的补充）

1. **OTA 回滚是真 bug，不是"防御性建议"**：审查日志说"缺 `esp_ota_mark_app_valid_cancel_rollback`"；实测本平台 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` → 不调它就会被回滚（"升完能用、断电又变老"）。
2. **重放窗"重启饿死"**：A5b 引入的收端窗口在"发送方重启（seq 回到 0）"时会一路判"过旧"，导致那台机器**永久发不进来**（上板刷单节点几乎必撞）→ 由 A5c 修掉（方案 A）。
3. **令牌桶的整数除法会丢余量**：`dt*PCT/100` 在 `dt<10ms`（高频发包、**正是限流最该起作用时**）时把余量全丢，控制桶永远补不回来 → 由 B-3b 改为余量累加。
4. **SF12 超看门狗担忧排除**：`sf` 校验本就是 7..11，最大帧（SF11/200B）≈4.0s < 看门狗 5s。

## 尚未做（明确边界，未动）

| 条目 | 说明 | 为什么没做 |
|---|---|---|
| P1-1 | 端到端传输层（端到端 ACK/窗口、QoS 真正落地） | 要动 `mav_router`/`mesh` 边界，属产品形态决策 |
| P1-3 余下 | **多 GCS 主从选举**（当前只计数不选举） | 产品形态决策 |
| P1-5 余下 | 网页/OTA **鉴权**（强制密码 / 一次性 PIN / 仅 USB 已枚举时可用） | 产品决策 |
| P1-6 | ~~CLI 够不着（单口文本分流 / 网页全接管 / TinyUSB 双 CDC，三选一）~~ → **已修（2026-10-15，选"单口文本分流"）**，见 M2（续）表 P1-6 行 | 产品决策 |
| P1-10 余下 | 丢弃/链路质量**上报到 MAVLink RADIO_STATUS**、`mac_send` "预约制"调度 | 属 P2 / 需先有端到端传输层 |
| P1-11 余下 | SX1268 / SX1262 / LLCC68 **驱动选型 A/B**（灵敏度/丢包/镜像抑制） | 需空口实测（步骤已写进 `todo.md`） |
| P1-12 余下 | `frag_tick` 独立时基、看门狗计数的意义、LED 规格（`led_states.md`） | 不在本轮范围 |
| P2 全表 | CI、单测、版本号、日志服务、出厂流程 | 下一阶段（M2） |

## 验证强度声明（重要）

本批全部改动**只到"编译通过"这一层**，另有若干**离线可验证**的部分做了真验证：
`airtime`（主机侧 15 项断言比对 Semtech 表）、重放窗位运算（逐分支手算）、令牌桶数学（dt=5ms 前后对比 + 溢出量级分析）、帧头偏移（编解码对称核对）、`rtos_tx_enqueue` 改写（与三个 `return` 分支等价比对）、`mav_router_handle_msg` 合并（与被替换调用逐行比对）、`setCurrentLimit` 与 `APP_ROLLBACK`（**从框架源码/sdkconfig 实证**，不是猜）。

**没有任何一项经过实机空口验证** —— 需要上板逐项复验的清单在仓库顶层 **`todo.md`**（含"看什么、什么算通过"），板子到货后按它走。

---

# M2 —— 把"应用 - 中间 - 驱动"从理念变成可执行约束

> 用户定调（2026-10-15）：架构理念 = **应用 / 中间 / 驱动** 三层；**现行目录即按此搭建，不重构结构**。产品级审查的目的是**减少上板调试时固件上的坑**；本项目是个人低成本开源项目，**不过度工程化** —— CI、单测基建、lint 门禁等已迁 `ROADMAP.md` 等待触发条件。
> 方案与拍板记录：`docs/plans/M2_APP_MID_DRV_PLAN.md`；进行中清单：`ROADMAP.md`。
> 流程变更：**任务书改为直接放 `docs/reviews/briefs/`**（原先放 `/tmp`，但 OpenCode 对工作目录以外的文件读取会被自动拒绝，`-f /tmp/...` 会 Read 失败）。

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **T-1** 分层依赖体检 | `af44ad3` | 新增 `tools/check_deps.py`：三层白名单式判依赖方向 + "越层动作"棘轮（白名单写在脚本里、各带一行理由，作为唯一事实来源）；默认**只报告、退出码 0**，`--strict` 留给未来 CI | **本地真跑**：硬规则违规 2 处（`link/adr.cpp:10`、`service/ota.cpp:14` 反向依赖 `app/role.h`）、越层动作违规 0、待收敛命中 6 处（`service/mav_router.cpp` 的 `crypto_seal_frame/frame_encode/rtos_tx_enqueue`） | 只覆盖字面 `#include` 与字面函数调用（宏、函数指针、头文件内 inline 检不到）；按用户定不接 CI |
| **T-2** 协议版本宣告 | `bb977f5` | 新增 `service/version.{h,cpp}`（基础支撑横切件）：`EM_FW_VERSION`（手工维护）+ `EM_PROTO_VERSION=2`（v1=14B 帧头 / v2=15B 帧头+AEAD）；**BEACON payload 空 → 1 字节协议版本**；收端比对不一致 → 计数 + 每个对端版本只告警一次 + 记录最近对端版本；**只告警不拒绝**；CLI 加 `ver` 命令 + `status` 协议行 | 编译过；逐行核对 payload 指针生命周期（存活到 `frame_encode` 拷贝完成）、信标不加密语义未变、`g_peer_proto` 取最后收到的信标 | **拦不住已经发生的 v1↔v2 不兼容**（旧固件连帧都解不出，走不到读 payload）；帧格式/封装语义每变一次必须 +1 |
| **T-3 / T-3b** 计数按三层归集 | `30f214a` | `status` 由"按添加顺序堆的十三行"改为**三层分组**：上下文行 → `--- 驱动层 ---` → `--- 中间层 ---`（链路/网络/速率）→ `--- 应用层 ---`（流量/系统）；**纯展示层重构**（不加计数、不换 getter、不动统计逻辑），归属理由写进代码注释（发送队列丢=中间层背压点；协议不一致只在链路行出现一次）；补回 `beacon_s`；行缓冲 128→384 | 编译过；逐项核对每个数字仍取原 getter、参数个数与顺序一致 | 中文列宽在不同终端未做对齐；`beacon_s` 仍是"仅显示、未接线" |

---

# M2（续）—— 分层约束落地 + 上板准备（2026-10-15）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **D2** 中间层不再反向依赖 `app/` | `c46793c` | 新增 `service/role_state.{h,cpp}`（基础支撑横切件）：本机角色的唯一真源；`app/role` 只负责**决定**角色（读 NVS + 选 sysid），中间层只**读**角色；`link/adr`、`service/ota` 改用 `role_state_get()` 并删掉对 `app/role.h` 的 include；`role_current()` 保留符号做包装（调用点零改动）。**审查发现**：角色**枚举**本来就在基础支撑 `nvs_cfg.h` 里，真正造成倒挂的只是"读角色的那个函数" | **`python3 tools/check_deps.py`：硬规则违规 2 处 → 0 处**（脚本自己跑出来的）；`pio run` 编译通过 | 体检脚本只覆盖字面 include 与字面调用（宏/函数指针/头文件 inline 检不到） |
| **FB / FB2** 批量烧录脚本 | 见提交 | `tools/flash_batch.sh`：合并镜像 → 逐块板 检测芯片 → **整片擦除** → 写入（带校验）→ 结果表；某块失败不中断其它板、退出码反映失败；`--merge-only` 无板也能验。FB2 补三处：① **把 `boot_app0.bin` vendor 进 `tools/`** 并写进 `0xe000`（否则工厂镜像与原生 upload 差一处，现场排查时是干扰项）② 擦除后 `sleep 1` + 写入失败重试一次（USB 桥重枚举）③ `MERGE_TIMEOUT` → `CMD_TIMEOUT` | **实测** `--merge-only`：944544 字节镜像、sha256 `bcfd48f1…`；**我独立复核**：镜像 `0xe000` 处 32 字节与 `tools/boot_app0.bin` **逐字节一致**；参数错误路径实测（无参 → 用法 + 退 2；`--all` 无串口 → 明确报错 + 退 1）。FB2 另用 `cmp` 证明"加 boot_app0 后镜像不变长、只差 12 个 otadata 有效字节" | **从未在真板上烧过**：擦除/写入/重试时序/460800 波特率全未上板验证 |
| **开机操作卡** | 见提交 | 新增 `docs/BRINGUP.md`：接线与备件（**先装天线再上电**）、串口权限、整片擦除、烧录命令、开机该看什么、`status` 分层怎么读、两机互通最小验证、常见现象对照表、安全红线 | 文档（无代码），与 `todo.md` / `ROADMAP.md` 交叉引用 | — |
| **SA 静态排查**（只读，未改任何代码） | 见提交 | 按 8 类"上板才炸"的模式全扫：定时器时基 / 时间回绕 / 任务栈 / 阻塞 / 中断 / 算术边界 / 动态内存 / 跨任务共享 | 先 grep 定规模 → 只读通读 → **助手逐条人工复核定判**（含在 **vendored RadioLib 源码**里核实 `transmit()` 超时 = 5×ToA）。报告：`docs/reviews/STATIC_AUDIT_2026-10-15.md`；**4 类干净负结果**（时间回绕无坑 / ~40 处临界区全成对 / 网络来的下标全有守卫 / 无裸动态分配） | **6 处真问题**（`frag_tick` 只在收帧超时跑、`mac_send` 免喂狗窗口 ≈4.6s 逼近 5s、`ota` WebServer 释放竞态、`adr_tick` 相位被阻塞、`g_stream` 跨核无锁、`enc_epoch` 8 位回绕）**待用户决定改哪几个**；4 条上板确认项已进 `todo.md` §7 |
| **P1-6** CLI 出口（**跑通阻断项**） | 见提交 | 查实"USB 双 CDC（`Serial`=CDC0、`Serial1`=CDC1）"假设**不成立**：本工程从未配 TinyUSB 复合描述符，`-DARDUINO_USB_MODE=1` 下 USB 只有一个 CDC，而 ESP32-S3 的 `Serial1` 是**未初始化的 UART1** → CLI 输出进黑洞、输入恒为 0；而**设角色的唯一入口就是 CLI**（网页要 WiFi、远程 cfg 要对面 GCS）→ 首次上电自锁。改为**单口分流**：`usb_task` 在字节层分开 MAVLink 帧（`0xFD`/`0xFE` 开头按帧收，v2 总长 = 10+len+2、v1 = 6+len+2）与 CLI 文本（256B 环形缓冲）；发送侧一把互斥量防止 CLI 文本插进MAVLink 帧中间；**500ms 卡帧保护**自恢复；`main.cpp` 无条件启动 USB 任务并删掉会抢字节的 `ROLE_UNSET` 回显 | 编译过；状态机**逐行人工核对**（帧长公式、缓冲边界、超时复位后当前字节仍按新起点处理、完整帧按 `FRAME_MTU` 分片投递）；`docs/BRINGUP.md` §3.1 新增"敲 `help` 有回显"两分钟自检 | **串口路由只能在真板验证**（PC 上验不了 USB 虚拟串口）；`cli.cpp` 未改（`usb_cli_read` 语义仍是逐字节）；MAVLink 签名帧未处理；`usb_cli_drop_count()` 未接入 `status` |
| **R3 第三轮审查（目标"能跑通"）** | 见提交 | 只查六条关键路径能否跑通，不追工程化：可编译 / 可烧录 / 可运行 / mesh 组网 / E22 收发 / 串口调试 | **编译**：清空 `.pio` 后全量重编 → SUCCESS（7.3s，RAM 17.4% / Flash 55.9%）；**烧录**：分区表在 4MB 内无重叠、app 0.86M « 1.5M、合并镜像偏移与分区表逐项对齐；**mesh**：转发/去重/TTL 不依赖角色、`failsafe_enter()` 只设状态不拦发、`duty_reserve` 启动态即干净窗口；**E22**：`Module(NSS,DIO1,NRST,BUSY)` 序、RF 开关、`begin(...,8,0.0f)` 参数序、OCP 在 begin 后、收发全在所有者任务；**串口**：飞控 UART `Serial0.begin(8N1, RX=44, TX=43)` 正确、CLI 已由 P1-6 打通 | **六条路径无阻塞性问题**；非阻塞项：① LBT/CAD 在该模块上可能恒"忙/错"→ mac 三次后硬发（不阻塞、吞吐降低；根因属驱动选型，见 `todo.md` 驱动 A/B）② `failsafe_policy.cpp:33` 注释仍提"CDC1"（纯注释陈旧）③ 上板才能定：模块是晶振还是 TCXO（`tcxoVoltage=0.0f` 是晶振假设）、DIO1 边沿、实际灵敏度 |

> 定位提醒（用户强调）：本项目是个人低成本开源项目，审查目的是**减少上板调试时固件上的坑**。
> **CI、单测基建、lint 门禁、RADIOSTATUS、三层递进产测、启动脚本化、日志落 flash、版本号自动生成** 都在 `ROADMAP.md` 里等触发条件；
> **分级日志**与**应用层发送封装收敛**明确**推后到第一轮上板之后**（前者要等真调试经验告诉我们该看什么，后者动的是发送主路径、必须能当场回归）。

| **R4 查官方文档 → 两条硬约束落地**（LB1 / SF1 / CMT1） | 见提交 | 起因：用户纠正"技术事实要查官方文档，不要凭记忆"（此前"LLCC68 不支持 CAD"是我凭记忆下的**错判**）。① 留档 `docs/references/LLCC68_E220-900M22S.md`（Semtech 数据手册 + 亿佰特规格书 + Zephyr binding + RadioLib 源码/issue #1329，全部附出处；PDF 原件存同目录并 **gitignore**）；② **驱动类 `SX1268` → `SX1262`**（RadioLib 的 `begin()` 按"芯片自报型号"逐字符校验：`SX1268` 类要 "SX1268"，而 E220 实测自报 "SX1261" → 会返回 **-2** 起不来；`SX1262` 类要的正是 "SX1261"）；③ **`ADR_SF_SLOW` 10 → 9**（LLCC68 硬约束：BW125 最高 SF9，而开机就停这一档，两机最小验证会哑）；④ 新增 `nvs_cfg_pair_ok()` 联动校验（**CLI `nvs_cfg_set` / 网页 `/set` / 整表 `nvs_cfg_save` 三处咽喉全拦**，`nvs_cfg_load` 对老配置降档 + 告警，网页/CLI 给明确报错）；⑤ 注释与文档全量同步（SF10→SF9、SX1268→SX1262）；**CAD 保留**（手册 §6.1.5 确认支持，此前判断作废） | `pio run` **SUCCESS**（RAM 17.4% / Flash 55.9%，+704B 即校验代码）；三个任务 diff 逐行审：除 `SX1262`/`ADR_SF_SLOW=9` 两处预期改动外**全是注释**；构建产物、忽略规则、PDF 归档均核对 | **上板待验**：① 驱动类能否过型号校验（若仍 **-2** 按 `SX1262`→`LLCC68`→`SX1268` 阶梯换）② SF9 慢速窗两台机能否互通（`ADR 对齐=是` 应在 5s 内出现）③ **TX 功率档位读数是否随设置变化**（SX1262 类用 SX1262 功率表，芯片是 LLCC68，可能不吻合 —— 并入 B-1 OCP 测试观察） |

| **R5 智谱 GLM-5.3 独立复审**（换模型交叉审查） | `85d2ee8` + 本提交 | 通读总架构 + **14 个主通路源文件逐行** + vendored RadioLib 关键函数（`transmit`/`finishTransmit`/`startTransmit`/`startReceiveCommon`/`startChannelScan`）+ 从零编译复验；产出根目录 `架构评价.md`（**总评 8.4/10**）与 `智谱介入审查漏洞.md` | **无致命漏洞**。新发现 **Z1**：`stats` 命令输出进 `Serial1` 黑洞（P1-6 单口分流改造**漏改的最后一处**）→ **已修**（改走 `usb_cli_write`，打印格式与内容一字未改）。**并纠正 GLM 的一条建议**：其"直接删 `stats` 命令"的前提（`status` 已覆盖全部信息）**不成立** —— 核出 `fwd`/`dup`(去重丢弃)/`reasm_to`(重组超时) 三项 `status` 并未覆盖，删命令会丢信息，故改为修出口 | **待上板**（已进 `todo.md` §7 并附判据）：**Z2**「发完能否收」（TX 完成边沿→DIO1→ISR→重挂接收这条链；2 行保险修法已备好）、**Z3** 切档瞬间的 ToA 精度；**顺手项（不单开提交，下次动到该文件再带）**：Z4 `mac_send` 返回值区分 LBT 状态、Z7 `failsafe_policy.cpp:33` 陈旧注释；**保持不动**：Z5/Z6/Z8/Z9；**后续可整理**：把 `fwd`/`dup`/`reasm_to` 并进 `status` 后即可删掉 `stats` 命令 |

