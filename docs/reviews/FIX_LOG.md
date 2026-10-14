# EMlink 修复对照表（FIX LOG）

> 审查基线：`CODE_REVIEW_2026-10-14.md`（原样保留，不修改）。工作流与目录说明见 `README.md`；每项的任务书在 `briefs/`。
> 提交号均为**本地提交**（截至 2026-10-15；推送状态见本文件末尾）。

## M0 —— 上板前阻断项（已完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **P0-1** 看门狗接错人 | `d475fd5` | 不再订阅 `loopTask`（Arduino 的 loopTask 只有 `enableLoopWDT()` 后才喂狗，本工程没调 → 上板必现 5s 复位循环）；rf/mac/mesh/mavlink/stats 各自 `watchdog_register_current()` + 循环内喂狗；rf 等 DIO1 通知带 1s 超时 | 编译过；逐个订阅任务的循环里都有 `watchdog_feed()` 核对 | 真"72h 无意外复位"要上板才成立 |
| **P0-4** 配置零校验 | `b01ff96` | `nvs_cfg` 集中校验（CLI/远程/网页共用同一入口；freq 862–930、sf 7–11、power -9~22、strtol/strtod 严格尾校验）；`node_id` 首次上电 MAC 派生并落盘 + 撞号计数；CLI/网页新增 `factory` 恢复出厂；`radio_is_ready` + rf_task 每 10s 重试 + 失败整灯红 | 编译过；校验范围与文档/架构 §9 一致性核对 | — |
| **P0-3** CAD 结果读早了 | `565af4c` | LBT 改状态驱动轮询（原实现启动后立刻读结果 → 永远判空闲，LBT 形同虚设）；超时按 SF/BW 推导（5 符号+3ms，SF7≈8ms / SF10≈43ms）；忙/错/超时一律按忙退避；等待期间补喂狗 | 编译过；超时公式手算 | 空口实测转发率 |
| **P0-2** 射频非单所有者 | `4355b14` | 所有 SPI/芯片操作收敛到 `rf_task`（`q_radio_hi/lo` 请求队列 + 任务通知回结果，支持所有者直执行）；新增 `radio_transmit_prio` 让 ACK 走高优先队列 | 编译过；上层调用方式零改动核对 | 上板并发压测 |

## M1 "A 档" —— 可脱离硬件验证的交付级修复（本轮完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **A1 / P1-2** 时延差 5~28× | `b658674` | 新增 `drivers/airtime`（Semtech ToA 整数实现，可主机单测）；占空比预算与 ACK 超时全按它推导（ACK 超时 = `max(NVS 值, ACK 帧 ToA + 50ms)`）；`duty_reserve` 对"单帧就超预算"放行并计数（否则 `mac_send` 死等）；`radio_transmit` 去掉被静默忽略的第三参 | **`airtime.cpp` 抽到主机侧编译跑 15 项断言**，与审查日志 ToA 表逐值吻合（16B@SF7=52ms、195B@SF7=313ms、16B@SF10=330ms、195B@SF10=1805ms…）；编译过 | 空口实测占空比与 ACK 成功率 |
| **A2 / P1-7** sysid 未接线 | `1cd1471` | 本机 sysid 改由 NVS 决定（`0` = 按角色默认）；新增"同 sysid 撞号"检测（按 `src_id != 本机 node_id` 排除自己中继回来的帧） | 编译过；`role_apply → nvs_cfg_load → mav_router_init` 链路核对；`role.cpp` 依赖 include 核对 | 多机同场实测 |
| **A3 / P1-8** 认证失败连转发一起吞 | `ef96ae2` | 解密/认证失败只跳过**本机投递**（`deliver=false`），不再 `continue` 吞掉转发；新增 `mesh_drop_auth_count()` | 编译过；另让 AI 逐处排查接收循环里其它 `continue` 是否也会连带吞转发（结论：无，都是设计内） | ACK 先于认证仍是 P1-1 范围 |
| **A4a / P1-9** 表满静默退化 | `8d52cc3` | 去重表满（16 源）改 LRU 淘汰最久未见者；route 加 `ROUTE_PATH_TTL_MS`(120s) 老化，`route_next_hop` 命中过期返回 0（上层回退广播，不发进死胡同）；addr_map 加 `ADDR_ENTRY_TTL_MS`(60s)；三表满淘汰/过期计数进 CLI status | 编译过；有符号差值比较（回绕安全）核对；所有新增读写都在 portMUX 临界区内核对 | TTL 参数待外场实测调 |
| **A4b / P1-9** 重组键漏 `src_id` | `5f717b3` | 重组键 `(orig_sysid, frag_seq)` → `(src_id, orig_sysid, frag_seq)`（同 sysid 的两机会串进同一槽、拼出坏帧喂飞控）；4 槽满时淘汰 `start_ms` 最早者并计数 | 编译过；键与槽字段一致性核对；被淘汰槽已 memset（不会再发陈旧 NACK）核对 | — |
| **A5a / P1-4** 帧头加世代号 | `f028795` | 帧头 14→15 字节，新增 `enc_epoch`；`nvs_cfg_next_enc_epoch()`（独立 NVS 键、每次开机 +1；**未动 `cfg_t`**——NVS 逐键存取，老配置天然兼容） | 编译过；编解码偏移对称核对（写 10/11/15 ↔ 读 10/11/15） | **协议不兼容**：两端固件必须同版本 |
| **A5b / P1-4** AEAD 加固 | `5cf0abe` | nonce 加 `enc_epoch`（原实现每次开机 `seq` 从 0 → 跨重启 nonce **必复用**，CCM keystream 复用）；AAD 7B = 逐跳不变帧头（`ttl`/`relay_id` 每跳都改，**必须排除**）；收端 8 源 × 64 位重放窗；`crypto_seal_frame` **先盖 epoch 再加密**（漏这步对端全部解密失败） | 编译过；**重放窗位运算逐分支手算**（整体左移/跨字进位/n≥64 清零/已见判定/回绕比较）；AAD 字段集与"中继只改 ttl·relay_id"的一致性核对 | epoch 仅 1 字节（同密钥跨 256 次开机后可能重现） |
| **A5c / P1-4** 重放窗"重启饿死" | `5f347a3` | 收端窗口按 `enc_epoch` 重同步：同源 epoch 变化 ⇒ 对方重启过 ⇒ 重置该源窗口并接受（epoch 被 AAD 绑定，伪造不了）；同 epoch 内仍是严格窗口；新增 `crypto_replay_resync_count()` | 编译过；自检 4 题（含"严格窗口有没有被削弱"） | 残risk：持有**旧世代**真密文者可触发一次窗口重置（用户已知情选方案 A） |
| **A6 / P1-5** OTA 不会确认固件 | `0c59ac6` | **实测框架 sdkconfig 确认平台启用了 `APP_ROLLBACK`** → 原实现从不调 `esp_ota_mark_app_valid_cancel_rollback()`，刷完重启会被 bootloader 悄悄回滚（"升完能用、断电又变老"）；新增 `ota_boot_confirm_tick()`（开机 10s 且 `radio_is_ready()` 才确认，射频起不来就不确认＝安全网）；维护模式空闲 5min / 硬上限 30min 自动退出（刷固件中让路） | 编译过；回滚开关是**从 sdkconfig 实证的**，不是猜测；自检 4 题 | 网页/OTA **仍无鉴权**（产品决策） |
| **A7 / P1-12** 死代码/未接线 | `4783e31` | 删 `q_frag`（白占 heap ≈1.7KB）/ `mac_should_ack()` / `ota_handle()` / `duty_set_max()`；补 `route_init()`/`addr_map_init()`/`frag_init()` 调用（此前从未被调用）；接线 `EV_CFG_CHANGED`（main loop 轮询重载内存镜像）与 `name`（status 显示）；`mav_router_feed` 改为调用 `mav_router_handle_msg`（消除重复实现） | 编译过；`mav_router_handle_msg` 与被替换调用逐行比对（等价）；删队列后 `rtos.h` 残留检查 | P1-12 第 1/3/4 条未做（见下） |

## 本轮新发现（审查日志之外的补充）

1. **OTA 回滚是真 bug，不是"防御性建议"**：审查日志说"缺 `esp_ota_mark_app_valid_cancel_rollback`"；实测本平台 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` → 不调它就会被回滚。已随 A6 修掉。
2. **重放窗"重启饿死"**：A5b 引入的收端窗口在"发送方重启（seq 回到 0）"时会一路判"过旧"，导致那台机器**永久发不进来**（上板刷单节点几乎必撞）。已由 A5c 修掉（用户拍板方案 A）。
3. **SF12 超看门狗担忧排除**：`sf` 校验本就是 7..11，最大帧（SF11/200B）≈4.0s < 看门狗 5s，不必再限制。

## 尚未做（明确边界，未动）

| 条目 | 说明 | 为什么没做 |
|---|---|---|
| P1-1 | 端到端传输层（端到端 ACK/窗口、QoS 落地） | 要动 `mav_router`/`mesh` 边界，属产品形态决策 |
| P1-3 | ADR 时间模型兜底与自愈（多跳对齐、失步检测） | 需多台实机实测 |
| P1-5 余下 | 网页/OTA **鉴权**（强制密码 / 一次性 PIN / 仅 USB 已枚举时可用） | 产品决策 |
| P1-6 | CLI 够不着（单口文本分流 / 网页全接管 / TinyUSB 双 CDC，三选一） | 产品决策 |
| P1-10 | 令牌桶 + 按类配额（控制流保底带宽） | 需先有端到端传输层（P1-1） |
| P1-11 | OCP 60→140mA、SX1268/SX1262/LLCC68 驱动选型 | 需空口实测灵敏度/丢包 |
| P1-12 余下 | `frag_tick` 独立时基、看门狗计数的意义、LED 规格（`led_states.md`） | 不在本轮 A 档范围 |
| P2 全表 | CI、单测、版本号、日志服务、出厂流程、链路状态上报 | 下一阶段（M2） |

## 验证强度声明（重要）

本批全部改动**只到"编译通过"这一层**，另有 `airtime`（主机单测）、重放窗（逐分支手算）、帧头偏移（对称核对）、`mav_router` 合并（等价比对）等**离线可验证**的部分做了真验证。
**没有任何一项经过实机空口验证**——板子到货后需按审查日志 §11 的三指标矩阵逐项复测（投递率/时延/断链恢复/OTA 成功率/多机并发/72h 长跑）。

## 推送状态

见仓库提交记录；`git log --oneline` 中 `P0-*` / `P1-*` 各条即本表条目。
