# EMlink 修复对照表（FIX LOG）

> 审查基线：`CODE_REVIEW_2026-10-14.md`（原样保留，不修改；里面多数问题已修，现状以本文件为准）。工作流与目录说明见 `README.md`；任务书不进仓库（源在 `~/EMlink-briefs/`，完成即归档到其 `done/`，规矩见 `briefs/README.md`）—— 长期记录只有本文件的对照表。

## M0 —— 上板前阻断项（已完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **P0-1** 看门狗接错人 | `d475fd5` | 不再订阅 `loopTask`（Arduino 的 loopTask 只有 `enableLoopWDT()` 后才喂狗，本工程没调 → 上板必现 5s 复位循环）；rf/mac/mesh/mavlink/stats 各自 `watchdog_register_current()` + 循环内喂狗；rf 等 DIO1 通知带 1s 超时 | 编译过；逐个订阅任务的循环里都有 `watchdog_feed()` 核对 | 真「72h 无意外复位」要上板才成立 |
| **P0-4** 配置零校验 | `b01ff96` | `nvs_cfg` 集中校验（CLI/远程/网页共用同一入口；freq 862–930、sf 7–11、power -9~22、strtol/strtod 严格尾校验）；`node_id` 首次上电 MAC 派生并落盘 + 撞号计数；CLI/网页新增 `factory` 恢复出厂；`radio_is_ready` + rf_task 每 10s 重试 + 失败整灯红 | 编译过；校验范围与文档/架构 §9 一致性核对 | — |
| **P0-3** CAD 结果读早了 | `565af4c` | LBT 改状态驱动轮询（原实现启动后立刻读结果 → 永远判空闲，LBT 形同虚设）；超时按 SF/BW 推导（5 符号+3ms，SF7≈8ms / SF10≈43ms）；忙/错/超时一律按忙退避；等待期间补喂狗 | 编译过；超时公式手算 | 空口实测转发率 |
| **P0-2** 射频非单所有者 | `4355b14` | 所有 SPI/芯片操作收敛到 `rf_task`（`q_radio_hi/lo` 请求队列 + 任务通知回结果）；新增 `radio_transmit_prio` 让 ACK 走高优先队列 | 编译过；上层调用方式零改动核对 | 上板并发压测 |

## M1 「A 档」 —— 可脱离硬件验证的交付级修复（已完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **A1 / P1-2** 时延差 5~28× | `b658674` | 新增 `drivers/airtime`（Semtech ToA 整数实现）；占空比与 ACK 超时全按它推导；`duty_reserve` 单帧超预算放行计数；`radio_transmit` 去掉被静默忽略的第三参 | `airtime.cpp` 抽到主机侧编译跑 15 项断言，与审查日志 ToA 表逐值吻合；编译过 | 空口实测占空比/ACK 成功率 |
| **A2 / P1-7** sysid 未接线 | `1cd1471` | 本机 sysid 由 NVS 决定（0=角色默认）；同 sysid 撞号检测（排除自己中继回来的帧） | 编译过；`role_apply → nvs_cfg_load → mav_router_init` 链路核对 | 多机同场实测 |
| **A3 / P1-8** 认证失败连转发一起吞 | `ef96ae2` | 解密失败只跳过本机投递，不再 `continue` 吞掉转发；`mesh_drop_auth_count()` | 编译过；逐处排查其它 `continue` 是否会连带吞转发（结论：无） | ACK 先于认证仍是 P1-1 范围 |
| **A4a / P1-9** 表满静默退化 | `8d52cc3` | 去重表 LRU 淘汰；route TTL 老化（过期 `route_next_hop` 返回 0 → 回退广播）；addr_map TTL；三表计数进 CLI | 编译过；有符号差值比较（回绕安全）核对；临界区闭合核对 | TTL 参数待外场调 |
| **A4b / P1-9** 重组键漏 `src_id` | `5f717b3` | 重组键加 `src_id`；4 槽满淘汰最早者并计数 | 编译过；键与槽字段一致性核对 | — |
| **A5a / P1-4** 帧头加世代号 | `f028795` | 帧头 14→15 字节，新增 `enc_epoch`；`nvs_cfg_next_enc_epoch()`（独立 NVS 键，未动 `cfg_t`） | 编译过；编解码偏移对称核对（10/11/15） | 协议不兼容：两端必须同版本 |
| **A5b / P1-4** AEAD 加固 | `5cf0abe` | nonce 加 epoch（原实现跨重启必复用）；AAD 7B=逐跳不变帧头；收端 8 源×64 位重放窗；seal 先盖 epoch | 编译过；重放窗位运算逐分支手算；AAD 字段集与转发路径核对 | epoch 仅 1 字节（256 次开机后可能重现） |
| **A5c / P1-4** 重放窗「重启饿死」 | `5f347a3` | 收端窗口按 `enc_epoch` 重同步：同源 epoch 变 ⇒ 对方重启 ⇒ 重置窗口并接受；同 epoch 内仍严格 | 编译过；自检 4 题（含「严格窗口有没有被削弱」） | 持有旧世代真密文者可触发一次窗口重置（用户拍板方案 A） |
| **A6 / P1-5** OTA 不确认固件 | `0c59ac6` | 实测 sdkconfig 确认平台启用了 APP_ROLLBACK → 原实现刷完重启会被悄悄回滚；新增 `ota_boot_confirm_tick()`（10s + 射频就绪才确认）；维护模式空闲 5min/硬上限 30min 自动退出 | 编译过；回滚开关从 sdkconfig 实证；自检 4 题 | 网页/OTA 仍无鉴权（产品决策） |
| **A7 / P1-12** 死代码/未接线 | `4783e31` | 删 `q_frag`/`mac_should_ack`/`ota_handle`/`duty_set_max`；补 `route/addr_map/frag_init()` 调用；接线 `EV_CFG_CHANGED` 与 `name`；`mav_router_feed` 改调 `handle_msg`（消重） | 编译过；`mav_router_handle_msg` 与被替换调用逐行比对（等价）；队列删除残留检查 | P1-12 第 1/3/4 条未做 |

## M1 「B 档」 —— 改法清楚、结论只能上板（本轮完成）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **B-1 / P1-11** OCP 过流保护 | `ff56082` | `begin()` 内部把 OCP 设成 60mA，而 +22dBm 需 120~140mA；功率 >14dBm 时在 `begin()` 之后调 `setCurrentLimit(140.0f)`（init + 重试两处） | 编译过；`setCurrentLimit` 签名与 `begin()` 里的 60.0 从 RadioLib 源码实证 | 驱动选型保守未动（默认仍 SX1268）；A/B 步骤写进 `todo.md` |
| **B-2 / P1-3** ADR 兜底与自愈 | `e1de762` | 信标 ttl 1→`MESH_BEACON_TTL`(2) 可中继一次；只有直收信标参与对齐；失步 15s（权威超时／只见多跳信标）→ 降级固定速率兼容模式 + 告警，收权威信标即自愈；多锚点只计数；对齐点按信标 ToA 补偿 | 编译过；信标最大跳数按 `ttl` + `mesh_ingress` 递减逐跳手推（确认 1 跳转发）；环形不绕圈=去重表+TTL 双保险核对 | 多 GCS 主从选举未做；兼容模式速率 SF7 是推断；补偿里的排队延迟是估算 |
| **B-3a/B-3b / P1-10** 令牌桶流量整形 | `992d646` | 新增 `service/ratelimit`：按 QoS 分桶（控制类保底 10%、用户类 90%，计价=空中毫秒）；准入即丢不阻塞（`MAC_DROP_RATE`）；单帧超桶放行；补充令牌改为余量累加；`mac_start` 调 `ratelimit_init` | 编译过；令牌数学逐项手算（含 dt=5ms 修复前后对比：0 → 正常）；与 duty 的层序/口径核对 | 份额参数（10%/30ms/100ms）待外场测；QOS1 与 QOS0 未细分 |
| **B-3c / P1-10** 各层丢弃计数 + 链路质量 | `b95da6e` | 4 个丢弃点加计数（发送队列/收包队列/请求队列/UART/USB——原来全静默丢）；`link_quality_pct` 由恒 0 改为 ACK 成功率；CLI 新增「丢包」行 | 编译过；`rtos_tx_enqueue` 改写后与三个 `return` 分支等价比对；各计数写入者的任务上下文核对（单写多读） | 上报到 MAVLink RADIO_STATUS 属 P2 |
| **B-3d / P1-10** MAVLink 流限速 | `a628568` | `mav_router` 按消息 ID 降频（SYS_STATUS 1s；GPS/ATTITUDE/位置/VFR_HUD 500ms；IMU/RC 200ms）；HEARTBEAT/COMMAND/PARAM/MISSION 永远放行；表外 msgid 一律放行；计数进 CLI「遥测降频」 | 编译过；限速表语义与「未知不擅自丢」策略核对；单线程上下文核对 | 阈值待实测；未区分上下行来源 |

## 上板 bring-up（2026-10-17，首块 ESP32-S3 开发板）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **S3** 4MB 板刷完启动崩溃 | 未提交 | `platformio.ini` 只设了 `board_build.flash_size`，而 esptool 的 `--flash_size` 取的是 board 的 `upload.flash_size`（`esp32-s3-devkitc-1` 默认 8MB）→ 镜像头声明 8MB、实板 4MB → `assert failed: do_core_init` 重启循环。补 `board_upload.flash_size = 4MB` + `board_upload.maximum_size = 4194304` | 上板：串口不再报 `Detected size(4096k) ... (8192k)`，正常启动到 CLI | 换 8MB 板需另建 env |
| **S4** 无射频模块时启动死锁 | 未提交 | ①`radio_call()` 等待加 3s 超时（原 `portMAX_DELAY`）；②请求队列改值拷贝（原指针 + 超时 = use-after-free，实测 `LoadProhibited`，`addr2line` 定位到 `rf_task_func`）；③从未成功过即判定「无模块」→`g_absent`，一次都不自动重试、所有射频操作立即失败；④只有 `g_ever_ready` 才 `watchdog_rf_fault()`；⑤启动日志与 CLI 明确报「未检测到射频模块（err=…）」 | 上板（无 E220）：启动到 `===== 就绪 =====`；`status` 显示 `模块=未检测到（err=-2）`、`wdt复位=0`、观察期零重启；`wifi` 起来 AP `EMlink-187 @ 192.168.4.1` | `begin()` 内部死等未根除（靠「无模块不重试」绕开）；超时后请求可能迟到执行；WiFi 页面功能待手机实测 |
| **S5** 上电自动进维护模式（条件式） | 未提交 | 新键 `auto_wifi_s`（默认 30，钳制 15–60、不可关闭）+ `ota_auto_tick()`：上电满 N 秒且 `mesh_rx_count / mesh_drop_crc_count / route_neighbors / mav_router_heartbeat_ok` 全无活动 → `ota_start()`；有活动则取消；只做一次（用户主动退出后不再被拉进） | 上板（无 E220）：`[ota] 上电 30s 无任何链路活动 → 自动进入 WiFi 维护模式` + `AP EMlink-187 @ 192.168.4.1` | 判据的漏判边界（极短活动/加密拒收帧）见任务书；网页侧编辑入口待做（S6） |
| **S6** 网页可编辑节点号/角色/维护延时 | 未提交 | `add_radio_fields()` 补 3 个字段（`node_id` / `role` 下拉含「未配置」 / `auto_wifi_s` 占满两列）；`handle_set()` 收这 3 个键（`auto_wifi_s` 与 nvs_cfg 同规则钳制、`role` 空串回退未配置）并在 `reboot=1` 时「回执→`ESP.restart()`」；`WUI_RADIO_B` 加「保存并重启」 | `design/verify_webui.py` 全通过（9593 B / 预留 10240、标签全闭合、关键元素齐）；`pio run` SUCCESS（Flash 56.1%）；上板自动进维护模式正常 | 网页侧的真实点到点验证待做（浏览器提交这三项、观察重启后生效）；页面余量仅 647 B，再加字段要先调 `WUI_RESERVE`；`node_id` 不做重复检测（用户明确） |
| **S7/S8** 网页「一律报格式错误」根治 + 表单全选项化 | 未提交 | S7：所有键先 `trim_ws()`（手机输入法/粘贴带首尾空白会让整串校验失败）+ 错误回执带 `键名 = 「原值」` + `/set` 打印收到的全部参数；S8：全部改选值控件 —— 频率=整数下拉(862–930)+小数下拉(0.05 步进)、功率下拉(10/14/17/20/22)、节点号滑块(1–254)、维护延时滑块(15–60)，仅密钥保留手输；`WUI_RESERVE` 10240→16384；`design/verify_webui.py` 同步模拟字段 | 上板实测（串口日志）：`[web] /set 收到 9 个参数：freq_int=894 freq_frac=0.75 … node_id=170 role=gcs` → 保存成功；第二次带 `reboot=1` → 板子重启 （此前是「一律格式错误」） | trims 不能解决内部空白/全角字符；错误提示把原值直接拼进 HTML 未转义（仅本机 AP 页面，风险低） |
| **S9–S16** 维护模式彩虹灯 + LED 状态机（颜色×节奏）+ 启动序列 | 未提交 | ①刷灯统一到 `stats_led_tick()`：上电序列（红→蓝→绿 轮闪 1.5s + 0.5s 灭灯空档）> 维护模式彩虹（21 档色表 = 7 基色 + 每段 2 插值，档间连续插值，2.5s 一轮，亮度 60）> 故障（红快闪 200ms）> 丢链（角色色快闪）> 待机（慢闪 1s）> 正常（常亮），500ms 去抖；②改独立 LED 任务（20ms、core1、优先级 1）+ LED 初始化提前到 `setup()` 开头 —— 修「上电约 8s 才开始闪」（真因：射频探测每个请求 3s 超时叠加 ~8s，而 LED 原在 setup 末尾才初始化、又靠 `loop()` 驱动）；③中继青→紫（与 AIR 的蓝区分）、未配置（文档写橙/实际黄）→ 白 | 上板（无 E220）：开机 ~0.3s 起红蓝绿轮闪 1.5s → 0.5s 全灭 → 红快闪（故障态）；彩虹用 `~/.local/bin/emlink_rainbow_gif.py` 离线出 GIF 先核对 | 「配置模式双闪」「看门狗复位红闪 1 下」仍无信号源、未实现；「待机」判据用邻居表，可能滞后；10ms/20ms 调度抖动会让相位有少量误差（用 `millis()` 算，不累积） |
| **E1/E2** EM Studio D-1（桌面调试台首版） | 未提交 | 新建 `tools/em-studio/`（pywebview 6.2.1 + pyserial，约 1200 行）：串口枚举 / 多设备标签页 / 连接断开（独占）/ 诊断台（CLI 直通 + 快捷命令 + 命令历史）；UI 同源复用 `webui.h` 的 CSS（`ui/shared.css` 与固件逐字符一致，桌面端修复只写 `app.css`）。E2 修三处：① WebKitGTK 的原生 `<select>` 必须 `appearance:none` 才吃深色样式（否则被 GTK 画成白底）+ 自绘 SVG 箭头；② 过滤 `ttyS*` 等主板串口噪音（只留 USB 类，加 `kind` 字段）；③ 空状态文案去重 | 真窗口实测（截图 + vision 核对）：下拉已是深色发丝线方框、枚举只剩两个 USB 设备、用户在界面敲 CLI 回显正常、`fuser` 证实串口被应用独占 | 参数区/结构化状态/MAVLink 编解码/烧录未做（D-2…D-5）；无自动重连（本版按端口名找设备，换口需手动刷新）；诊断台日志区无内容时缺提示文案 |
| **F1–F11** 频率三位精度 + EM Studio D-2/参数区/三角特效 + 固件灯态查询与推送 | 未提交 | ①`nvs_cfg_get` 的 freq 改 `%.3f`（原 `%.1f`，读一次丢一次精度）；网页与桌面端频率均改 整数 + 十分位 + 百分位 + 千分位 四下拉（桌面端回填/合成走字符串，避浮点误差）；②EM Studio 参数区（角色/节点号/频率/SF/BW/CR/功率/维护延时/密钥；改动即 `set`、保存才 `save`、支持保存并重启）；③固件加 `led` 查询命令 + 灯态变化主动推送 `#led …`（默认开、50ms 防抖、`led stream off` 可关）；④诊断台标题横线中央的倒立三角与板子灯同步：后端只去驱动三角、不显示、不刷屏，前端按 `half` 用 CSS 动画瞬时切换（`steps(1,end)`，去掉会被打断的 transition）；⑤修：空状态文案按设备数动态（原写死「没有检测到串口设备」与实际矛盾） | 上板实测：`led mode=fault color=255,0,0 half=100`；推送节奏 ≈100ms/条（红快闪）、彩虹 ≈22ms/条；用户验收通过 | 不保证与板子严格同相（两端时钟不同 + 链路延迟；要同相需固件在推送里带 `millis()` 做时钟映射）；彩虹推送 ~50 行/秒偏密（占串口带宽约 17%），可降至 8–12 行/秒 |
| **D3a/F12/F13** 结构化状态台（status 三层卡片）+ 紧凑排版 + 溢出修复 | 未提交 | ①`cli_client.query` 加 `silent`（轮询 `status` 时回显与输出不进诊断台，只交给解析器 → 不刷屏）；②`prober.read_status` 解析成 `layers[{name,items[{label,value,level}]}]`；③level 规则：err = 值含「失败/未检测到/否/err=-N」；warn = 字段名含 丢/掉/失败/超限/过期/淘汰/撞号/降级/不一致/失步/重放/重传/拒绝 且值 >0；参数白名单（heap/freq/sf/bw/cr/power/ttl/retries/beacon_s…）永不 warn，且用完整字段名匹配（避免 `ttl` 吞掉 `TTL丢弃`）；④UI 三层卡片 + 每 2 秒静默刷新 + 条目局部更新（不重建 DOM）；⑤F12 极致集成排版（`auto-fill minmax(172px,1fr)` 网格、11px 字号、一行 3–4 指标、去掉卡区滚动）；⑥F13 修条目溢出（flex 项 `min-width:0` 才让 `ellipsis` 生效 + 值 `max-width:58%` + 行级 `overflow:hidden` 兜底 + hover `title`） | 用户验收：「很牛了，可以」 | 未用真板做过完整 `status` 解析回归（只用 `cli.cpp` 的真实格式样例）；`路由过期=0/淘汰=0` 这类组合值按关键词整体判定，未拆细 |
| **D3b/F14/F15** MAVLink 强行编解码（上位机）+ 状态卡修 `#led` 混入 + 固件 USB 收帧计数 | 未提交 | ①`backend/mav_raw.py`：MAVLink v2 组帧/拆帧 + CRC-16/MCRF4XX；`crc_extra` 查方言表有则用真值、无则用 0（与固件 `mav_crc_ok` 一致，故自造消息能过）；拆帧容错（找 `0xFD` 按 len 取全长，不足留尾巴下次拼），CRC 错也照样解出来并标 `crc_ok=False`（这才是「强行」）；`_apply_target()` 从方言表读 `target_system` 偏移直接改 payload；②`app.py` 桥方法 `api_set_mode` / `api_send_frame`（`_int_arg` 范围校验：msgid 0..0xFFFFFF、sysid/compid/target 0..0xFF）/ `api_mav_poll`；`serial_link` 读线程按模式分流（mavlink→帧解析器，否则→CLI），帧增量上限 1000；③UI 解码面板：发送表单（payload 文本/hex + msgid/sysid/compid/target）+ 消息表（一行一帧含 hex/文本还原/CRC 通过与否）+ 选中看完整 payload + 模拟心跳（HEARTBEAT 每秒）+ 请求参数（`PARAM_REQUEST_LIST` 广播）；顶部「配置/解码」热切换不断开；④F14 `prober` 跳过 `#` 开头的行 —— 板子灯态推送经 `> ` 回显前缀混进应用层卡片的垃圾项（实测从 7 项回到 6 项）；⑤F15 固件 `mav_router` 加 USB 侧收帧计数（`usb_rx`=CRC 通过 / `usb_bad`=CRC 失败，只在 `SRC_USB` 自增、不改原有路由行为），`status` 中间层显示 `MAV: USB收帧=0 校验失败=0` | 实测通过（F15）：发 3 条正确帧 + 1 条故意改坏 CRC 的帧 → `USB收帧 0→3`、`校验失败 0→1` → 证明板子确实在解析电脑发来的 MAVLink 帧（自造 msgid 200/201/202 全部 CRC 通过）；帧自解自洽（HEARTBEAT 取到真 `crc_extra=50`，自造消息取 0）；真板 `status` 解析 31 项三层正确、`heap`/`SF`/`beacon_s` 不误黄、`模块=未检测到`/`ADR 对齐=否` 标红；刷机（只写不擦）后配置保留 | 空口端到端未验（等 E220；板子 MAVLink 源头是飞控串口，无对端时消息表必为空）；假同步头（payload 含 `0xFD`）会错位且 CRC 失败后不回溯重同步；切解码模式仍需手动 `led stream off` 才能去掉 `#led` 噪声（自动关选项已议、未做） |
| **F16** 封装内 flash 的 SuperMini 在 DIO 下固件永不启动 → 必须 QIO | 未提交 | 现象：新板（MAC `98:c3:77:60:c1:b4`）烧录成功、`esptool` 能识别芯片、读 flash 三处内容全是有效镜像（`0x0` bootloader `e9…`、`0x8000` 分区表 `0x50AA…`、`0x10000` app `e9…`），但串口零字节、固件永不启动；该板跑厂商出厂固件时 USB 每 3 秒枚举一次（证明硬件能跑固件）。根因：`board = esp32-s3-devkitc-1` 默认 flash 模式是 DIO，而这块板是封装内 flash（`Embedded Flash 4MB (XMC)` + `Embedded PSRAM 2MB` 的 S3 型号），DIO 下 bootloader 读不到 app。修法：`platformio.ini` 显式 `board_build.flash_mode = qio`（quad flash 全批次支持；另一块板是外挂 flash，DIO 也能跑，所以此坑长期未暴露） | 实测：改 QIO + 整片擦除重刷后立刻恢复（`#led` 灯态推送 + CLI `status` 全正常，连读 3 次稳定）；换回 DIO 则再次静默。排除项：①PSRAM 假设 —— 去掉 `-DBOARD_HAS_PSRAM` 单独编译刷入仍静默，且 esptool 报该板确有 2MB PSRAM；②烧录不完整 —— 已逐段读回核对；③USB 硬件 —— 出厂固件能反复枚举 | 已出货、仍跑 DIO 固件的板子，日后 OTA 可能出现「新 bootloader 用 QIO、旧固件 DIO」不一致，建议统一重刷；`board_build.flash_mode` 的具体取值对不同批次是否都最优未逐批验证（本次只验证了 QIO 对两块板均可用） |
| **D-4** EM Studio 内置 USB 烧录 | 未提交 | ①`backend/flasher.py`：`find_esptool()`（PATH 查找，`ESPTOOL` 环境变量可覆盖）+ `flash()` 用 `subprocess.Popen` + 读线程逐行解析进度（正则 `(\d+)\s*%`）；只写 app = `write_flash --flash_mode qio --flash_size 4MB 0x10000 <bin>`（QIO 显式写死，见 F16），整片 = 先 `erase_flash` 再写 `0x0`；擦除+写入共用一个 deadline，超时 kill 子进程；不用 `shell=True`；②`app.py`：`api_pick_firmware`（系统文件对话框）/ `api_flash`（后台线程，先 `link.close()` 让出串口，结束后复用既有 connect 逻辑重连；同一时刻只允许一个烧录）/ `api_flash_progress`（300 ms 轮询，仅在 `running` 时）/ `api_default_firmware`（默认指向 `.pio/build/<env>/firmware.bin`）；③UI：设备面板「固件烧录」区块（选文件 + 路径 + 整片烧录勾选（会清空配置，需选合并镜像） + 开始 + 进度条 + 状态），烧录中禁用连接/断开/刷新/选文件，关闭标签也被拦截 | 真机验证通过：用它刷 `ttyACM0`（`98:C3:77:60:C4:48`）→ 进度 11%→100% 平滑、7.0 s 完成、`ok=True`；刷完板子正常启动（`射频: 就绪=是 模块=在线`）、新固件的 `MAV: USB收帧` 出现（该板此前没有）、只写 app 故 `node_id=237` 配置保留 | 整片烧录路径未实跑（需合并镜像）；未测 esptool 握手失败/端口被占的报错文案；UI 的文件对话框与进度条只做过 mock，未在真窗口点过（用户可自验） |
| **D-5** GUI 打磨：会话日志导出 + 诊断归因（不加动效） | 未提交 | ①`prober.diagnose(layers)`：表驱动规则表（模块未检测到 / 就绪否 / 认证失败 / 协议不一致 / 重放丢弃 / TTL或去重 / 发送队列丢 / sysid撞号 / node_id撞号 / wdt复位 / ADR未对齐），同层只留最具体一条、err 排前、无命中返回空列表（不出「一切正常」这种废话）；②`app.py.api_export_log(device_id, kind, console_lines, frames, …)`：`SAVE_DIALOG` 选路径、取消静默返回 `cancelled`；默认名 `EMStudio_<node_id>_<yyyymmdd-HHMMSS>.txt`；txt 含会话头 / 状态快照 / 诊断台逐行（`[HH:MM:SS]` 前缀）/ 解码消息，json 结构化；console ≤5000 行、frames ≤200 条取最近；③UI：诊断台「导出日志」、解码面板「导出 JSON」、状态卡上方 `#diag-verdict` 归因行（无归因时隐藏、随状态卡局部更新）；④明确不加动效/过渡/阴影/圆角（沿用 ELRS 最简几何风） | 实测：`diagnose` 五组场景全对（正常→空、射频缺失→驱动层、认证失败→中间层、协议不一致+撞号→两条跨层、TTL+去重→合并一条环路结论）；真板单机 34 项 → 无归因。并修掉一处误报：单机无邻居时 `ADR 对齐=否` 属正常，规则改为必须有邻接活动（信标/多跳/多锚点 >0）才报 | 导出对话框与落盘未在真窗口点过（pywebview/GTK 版本差异）；归因只覆盖规则表内的项，未列出的非零计数不会自动出结论；解码模式下导出的状态快照是最近一次 CLI 状态，不保证实时 |
| **D-6** EM Studio「让出串口」能力（SIGUSR1 + 界面开关） | 未提交 | ①`app.py`：启动写 `~/em-studio.pid`、主线程注册 `SIGUSR1`（handler 只 `Event.set()`，不在信号上下文做 I/O）、守护线程被唤醒后遍历设备 `link.close()`（保留标签页对象，不是 `close_permanently()`）并用 `evaluate_js` 通知前端；退出删 PID 文件（校验内容等于自身 PID，防误删别的实例）；桥方法 `api_set_external_release` / `api_get_external_release`；②UI 顶栏「允许外部释放串口」复选框（默认勾选）—— 取消勾选后信号被忽略（保护用户手动操作时不被踢）；③外部释放后标签页转「未连接」、诊断台追加灰字 `[外部请求] 已释放串口` | 子进程实测：`SIGUSR1` → 串口被 `close()`、设备对象保留、开关关闭时信号被忽略、PID 文件启动写入/退出删除；`py_compile` + `node --check` 通过 | 未在真实 pywebview 窗口验证守护线程调 `evaluate_js` 的线程行为；未用真实串口跑过 `kill -USR1`（仅用假 link）；烧录进行中收到信号会与烧录流程竞争，未测；`kill -9` 会残留 PID 文件（外部脚本需先 `kill -0` 校验） |
| **F19** RXEN/TXEN 引脚接反 → 空口完全不通（而 SPI/状态全绿） | 未提交 | 现象：两块板 SPI 正常（`模块=在线`、`就绪=是`）、参数完全一致（915/SF7/BW125/CR5/sync18）、供电稳定（3.337/3.338 V）、模块型号确认 `E220-900M22S`、天线已接、距离已拉开、关掉 ADR 仍不通；`ping` 双向超时、`信标=0`、ADR 永远协商不上；`radio_tx` 不报错（`wdt复位=0`、`射频重建=0`）。根因：`radios/e220-900m22s.h` 里 `PIN_LORA_RXEN=1 / PIN_LORA_TXEN=2` 与实物相反（GPIO1 实为 TXEN、GPIO2 实为 RXEN）→ 发射时打开接收开关、接收时打开发射开关，射频开关永远处于错误状态。定位手段：新增 `gpiotest` 命令（强制四段电平：TXEN高/RXEN高/两脚高/恢复）+ 万用表实测 —— 强制 `TXEN=HIGH` 时升到 3.3 V 的却是 GPIO1 → 判定接反。修法：交换两个宏，并在代码注释与技术档案标注「实测修正，换批次需用 `gpiotest` 复核」 | 实测通过：修引脚 + 冷启动后 `ping 187` → `通：rtt=130ms rssi=-98dBm`；MAVLink 端到端（GCS 的 USB 灌帧 → LoRa → 对端 USB）解出 4 条自造帧（msgid 200/201/202，payload 文本正确还原，CRC 全 OK） | RSSI 读数经后续验证为无效（见下一行 F19 补充：不随距离变化，1~2 m / 4 m / 15 m 均为 −98 或 −83）→ 不可据此判断天线效率，此前「天线效率低」结论已作废；`发送队列丢=33` 是灌帧过快（20 帧/秒 vs LoRa 每帧 ~100 ms）导致，非故障；`set sf 12` 被固件按 LLCC68 的 SF×BW 配对规则拒绝（BW125→SF≤9），故测试实际在 SF7 完成 |
| **F19 补充** 链路实测距离 + RSSI/SNR 交叉验证 | 未提交 | 实测距离（一 GCS 一 AIR、SF7、BW125、20 dBm、室内）：穿墙 + 拐弯的 NLOS 条件下 15 m 稳定可通（`ping` 3/3 成功，RTT 130 ms）—— 不是直线视距：要穿过宿舍门/墙并拐进大厅，理论损耗约 105~120 dB，SF7 灵敏度 −123 dBm 仍有 20 dB 余量，链路实际可用。重要发现：`ping` 回报的 RSSI 不随距离变化 —— 1~2 m = −83 dBm、4 m = −98 dBm、15 m 仍是 −98.0 dBm 且波动 0（不同距离给出完全相同的值）→ 该 RSSI 读数不可信，可能是缓存值/未取本次帧的包 RSSI/钳位。因此此前「天线效率低 40 dB」的推断作废（那是基于不可信读数得出的） | 15 m 稳定通联 | `scan` 命令交叉验证(关键更正)：`ping` 之后立刻 `scan` → `最近收包 rssi=-98 snr=+10`。SNR +10 dB 与 RSSI −98 dBm 完全自洽（噪声底 = −98 − 10 = −108 dBm，正是 LoRa 典型底噪），且 SF7 只要 SNR > −7.5 dB 即可解调 → RSSI 读数是真实的，不是假值；此前「RSSI 无效」的判断作废（当时只有 RSSI 单一数据，无从交叉验证）。4 m 与 15 m 同为 −98 是因为两条路径损耗相近（15 m 那次穿墙拐弯，4 m 时可能已在穿墙路径上）；1~2 m 视距时为 −83，随距离有变化。当前链路质量良好，远未到解调门限。 | 15 m 稳定通联，SNR +10 dB | 更远距离（30 m / 50 m）未测；`邻居` 列表为空（`scan` 显示「邻居：无」）—— 因 `adr=off` 不发信标，邻居表靠信标/路由帧填充，属预期；SF 只能到 9（BW125 限制），要更远需改 BW（BW250→SF10、BW500→SF11） |
| **D-6b** 修正释放信号：`SIGUSR1` → `SIGRTMIN+3` | 未提交 | 现象：D-6 上线后用户报「EM Studio 又被关了」。根因：日志 `Overriding existing handler for signal 10` —— `SIGUSR1` = signal 10，而 WebKit / JavaScriptCore 自己用它做 JS 引擎的 GC 通知；抢占后 JSC 的垃圾回收失效，进程崩溃退出。修法：改用 Linux 实时信号 `SIGRTMIN+3`（实测本机 = 37），不与任何常规信号冲突 | 实测：`kill -37 <pid>` → 程序存活、日志输出「已释放 1 个串口」 | 日志里残留的 `SIGUSR1` 文案已改为「外部释放信号」；教训：给 GUI 程序装信号处理前，先查框架（WebKit/Qt 等）占用了哪些信号，`SIGUSR1/SIGUSR2` 都可能被占用 |

## 本轮新发现（审查日志之外的补充）

1. OTA 回滚是真 bug，不是「防御性建议」：审查日志说「缺 `esp_ota_mark_app_valid_cancel_rollback`」；实测本平台 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` → 不调它就会被回滚（「升完能用、断电又变老」）。
2. 重放窗「重启饿死」：A5b 引入的收端窗口在「发送方重启（seq 回到 0）」时会一路判「过旧」，导致那台机器永久发不进来（上板刷单节点几乎必撞）→ 由 A5c 修掉（方案 A）。
3. 令牌桶的整数除法会丢余量：`dt*PCT/100` 在 `dt<10ms`（高频发包、正是限流最该起作用时）时把余量全丢，控制桶永远补不回来 → 由 B-3b 改为余量累加。
4. SF12 超看门狗担忧排除：`sf` 校验本就是 7..11，最大帧（SF11/200B）≈4.0s < 看门狗 5s。

## 尚未做（明确边界，未动）

| 条目 | 说明 | 为什么没做 |
|---|---|---|
| P1-1 | 端到端传输层（端到端 ACK/窗口、QoS 真正落地） | 要动 `mav_router`/`mesh` 边界，属产品形态决策 |
| P1-3 余下 | 多 GCS 主从选举（当前只计数不选举） | 产品形态决策 |
| P1-5 余下 | 网页/OTA 鉴权（强制密码 / 一次性 PIN / 仅 USB 已枚举时可用） | 产品决策 |
| P1-6 | ~~CLI 够不着（单口文本分流 / 网页全接管 / TinyUSB 双 CDC，三选一）~~ → 已修（2026-10-15，选「单口文本分流」），见 M2（续）表 P1-6 行 | 产品决策 |
| P1-10 余下 | 丢弃/链路质量上报到 MAVLink RADIO_STATUS、`mac_send` 「预约制」调度 | 属 P2 / 需先有端到端传输层 |
| P1-11 余下 | SX1268 / SX1262 / LLCC68 驱动选型 A/B（灵敏度/丢包/镜像抑制） | 需空口实测（步骤已写进 `todo.md`） |
| P1-12 余下 | `frag_tick` 独立时基、看门狗计数的意义、LED 规格（`led_states.md`） | 不在本轮范围 |
| P2 全表 | CI、单测、版本号、日志服务、出厂流程 | 下一阶段（M2） |

## 验证强度声明（重要）

本批全部改动只到「编译通过」这一层，另有若干离线可验证的部分做了真验证：
`airtime`（主机侧 15 项断言比对 Semtech 表）、重放窗位运算（逐分支手算）、令牌桶数学（dt=5ms 前后对比 + 溢出量级分析）、帧头偏移（编解码对称核对）、`rtos_tx_enqueue` 改写（与三个 `return` 分支等价比对）、`mav_router_handle_msg` 合并（与被替换调用逐行比对）、`setCurrentLimit` 与 `APP_ROLLBACK`（从框架源码/sdkconfig 实证，不是猜）。

**没有任何一项经过实机空口验证** —— 需要上板逐项复验的清单在仓库顶层 `todo.md`（含「看什么、什么算通过」），板子到货后按它走。


---

# M2 —— 把「应用 - 中间 - 驱动」从理念变成可执行约束

> 用户定调（2026-10-15）：架构理念 = 应用 / 中间 / 驱动 三层；现行目录即按此搭建，不重构结构。产品级审查的目的是减少上板调试时固件上的坑；本项目是个人低成本开源项目，不过度工程化 —— CI、单测基建、lint 门禁等已迁 `ROADMAP.md` 等待触发条件。
> 方案与拍板记录：`docs/plans/M2_APP_MID_DRV_PLAN.md`；进行中清单：`ROADMAP.md`。
> 流程变更：任务书改为直接放 `docs/reviews/briefs/`（原先放 `/tmp`，但 OpenCode 对工作目录以外的文件读取会被自动拒绝，`-f /tmp/...` 会 Read 失败）。

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **T-1** 分层依赖体检 | `af44ad3` | 新增 `tools/check_deps.py`：三层白名单式判依赖方向 + 「越层动作」棘轮（白名单写在脚本里、各带一行理由，作为唯一事实来源）；默认只报告、退出码 0，`--strict` 留给未来 CI | 本地真跑：硬规则违规 2 处（`link/adr.cpp:10`、`service/ota.cpp:14` 反向依赖 `app/role.h`）、越层动作违规 0、待收敛命中 6 处（`service/mav_router.cpp` 的 `crypto_seal_frame/frame_encode/rtos_tx_enqueue`） | 只覆盖字面 `#include` 与字面函数调用（宏、函数指针、头文件内 inline 检不到）；按用户定不接 CI |
| **T-2** 协议版本宣告 | `bb977f5` | 新增 `service/version.{h,cpp}`（基础支撑横切件）：`EM_FW_VERSION`（手工维护）+ `EM_PROTO_VERSION=2`（v1=14B 帧头 / v2=15B 帧头+AEAD）；BEACON payload 空 → 1 字节协议版本；收端比对不一致 → 计数 + 每个对端版本只告警一次 + 记录最近对端版本；只告警不拒绝；CLI 加 `ver` 命令 + `status` 协议行 | 编译过；逐行核对 payload 指针生命周期（存活到 `frame_encode` 拷贝完成）、信标不加密语义未变、`g_peer_proto` 取最后收到的信标 | 拦不住已经发生的 v1↔v2 不兼容（旧固件连帧都解不出，走不到读 payload）；帧格式/封装语义每变一次必须 +1 |
| **T-3 / T-3b** 计数按三层归集 | `30f214a` | `status` 由「按添加顺序堆的十三行」改为三层分组：上下文行 → `--- 驱动层 ---` → `--- 中间层 ---`（链路/网络/速率）→ `--- 应用层 ---`（流量/系统）；纯展示层重构（不加计数、不换 getter、不动统计逻辑），归属理由写进代码注释（发送队列丢=中间层背压点；协议不一致只在链路行出现一次）；补回 `beacon_s`；行缓冲 128→384 | 编译过；逐项核对每个数字仍取原 getter、参数个数与顺序一致 | 中文列宽在不同终端未做对齐；`beacon_s` 仍是「仅显示、未接线」 |


---

# M2（续）—— 分层约束落地 + 上板准备（2026-10-15）

| 条目 | 提交 | 改了什么（要点） | 怎么验证的 | 遗留 |
|---|---|---|---|---|
| **D2** 中间层不再反向依赖 `app/` | `c46793c` | 新增 `service/role_state.{h,cpp}`（基础支撑横切件）：本机角色的唯一真源；`app/role` 只负责决定角色（读 NVS + 选 sysid），中间层只读角色；`link/adr`、`service/ota` 改用 `role_state_get()` 并删掉对 `app/role.h` 的 include；`role_current()` 保留符号做包装（调用点零改动）。审查发现：角色枚举本来就在基础支撑 `nvs_cfg.h` 里，真正造成倒挂的只是「读角色的那个函数」 | `python3 tools/check_deps.py`：硬规则违规 2 处 → 0 处（脚本自己跑出来的）；`pio run` 编译通过 | 体检脚本只覆盖字面 include 与字面调用（宏/函数指针/头文件 inline 检不到） |
| **FB / FB2** 批量烧录脚本 | 见提交 | `tools/flash_batch.sh`：合并镜像 → 逐块板 检测芯片 → 整片擦除 → 写入（带校验）→ 结果表；某块失败不中断其它板、退出码反映失败；`--merge-only` 无板也能验。FB2 补三处：① 把 `boot_app0.bin` vendor 进 `tools/` 并写进 `0xe000`（否则工厂镜像与原生 upload 差一处，现场排查时是干扰项）② 擦除后 `sleep 1` + 写入失败重试一次（USB 桥重枚举）③ `MERGE_TIMEOUT` → `CMD_TIMEOUT` | 实测 `--merge-only`：944544 字节镜像、sha256 `bcfd48f1…`；我独立复核：镜像 `0xe000` 处 32 字节与 `tools/boot_app0.bin` 逐字节一致；参数错误路径实测（无参 → 用法 + 退 2；`--all` 无串口 → 明确报错 + 退 1）。FB2 另用 `cmp` 证明「加 boot_app0 后镜像不变长、只差 12 个 otadata 有效字节」 | 从未在真板上烧过：擦除/写入/重试时序/460800 波特率全未上板验证 |
| **开机操作卡** | 见提交 | 新增 `docs/BRINGUP.md`：接线与备件（先装天线再上电）、串口权限、整片擦除、烧录命令、开机该看什么、`status` 分层怎么读、两机互通最小验证、常见现象对照表、安全红线 | 文档（无代码），与 `todo.md` / `ROADMAP.md` 交叉引用 | — |
| **SA 静态排查**（只读，未改任何代码） | 见提交 | 按 8 类「上板才炸」的模式全扫：定时器时基 / 时间回绕 / 任务栈 / 阻塞 / 中断 / 算术边界 / 动态内存 / 跨任务共享 | 先 grep 定规模 → 只读通读 → 助手逐条人工复核定判（含在 vendored RadioLib 源码里核实 `transmit()` 超时 = 5×ToA）。报告：`docs/reviews/STATIC_AUDIT_2026-10-15.md`；4 类干净负结果（时间回绕无坑 / ~40 处临界区全成对 / 网络来的下标全有守卫 / 无裸动态分配） | 6 处真问题（`frag_tick` 只在收帧超时跑、`mac_send` 免喂狗窗口 ≈4.6s 逼近 5s、`ota` WebServer 释放竞态、`adr_tick` 相位被阻塞、`g_stream` 跨核无锁、`enc_epoch` 8 位回绕）待用户决定改哪几个；4 条上板确认项已进 `todo.md` §7 |
| **P1-6** CLI 出口（跑通阻断项） | 见提交 | 查实「USB 双 CDC（`Serial`=CDC0、`Serial1`=CDC1）」假设不成立：本工程从未配 TinyUSB 复合描述符，`-DARDUINO_USB_MODE=1` 下 USB 只有一个 CDC，而 ESP32-S3 的 `Serial1` 是未初始化的 UART1 → CLI 输出进黑洞、输入恒为 0；而设角色的唯一入口就是 CLI（网页要 WiFi、远程 cfg 要对面 GCS）→ 首次上电自锁。改为单口分流：`usb_task` 在字节层分开 MAVLink 帧（`0xFD`/`0xFE` 开头按帧收，v2 总长 = 10+len+2、v1 = 6+len+2）与 CLI 文本（256B 环形缓冲）；发送侧一把互斥量防止 CLI 文本插进MAVLink 帧中间；500ms 卡帧保护自恢复；`main.cpp` 无条件启动 USB 任务并删掉会抢字节的 `ROLE_UNSET` 回显 | 编译过；状态机逐行人工核对（帧长公式、缓冲边界、超时复位后当前字节仍按新起点处理、完整帧按 `FRAME_MTU` 分片投递）；`docs/BRINGUP.md` §3.1 新增「敲 `help` 有回显」两分钟自检 | 串口路由只能在真板验证（PC 上验不了 USB 虚拟串口）；`cli.cpp` 未改（`usb_cli_read` 语义仍是逐字节）；MAVLink 签名帧未处理；`usb_cli_drop_count()` 未接入 `status` |
| **R3 第三轮审查（目标「能跑通」）** | 见提交 | 只查六条关键路径能否跑通，不追工程化：可编译 / 可烧录 / 可运行 / mesh 组网 / E22 收发 / 串口调试 | 编译：清空 `.pio` 后全量重编 → SUCCESS（7.3s，RAM 17.4% / Flash 55.9%）；烧录：分区表在 4MB 内无重叠、app 0.86M « 1.5M、合并镜像偏移与分区表逐项对齐；mesh：转发/去重/TTL 不依赖角色、`failsafe_enter()` 只设状态不拦发、`duty_reserve` 启动态即干净窗口；E22：`Module(NSS,DIO1,NRST,BUSY)` 序、RF 开关、`begin(...,8,0.0f)` 参数序、OCP 在 begin 后、收发全在所有者任务；串口：飞控 UART `Serial0.begin(8N1, RX=44, TX=43)` 正确、CLI 已由 P1-6 打通 | 六条路径无阻塞性问题；非阻塞项：① LBT/CAD 在该模块上可能恒「忙/错」→ mac 三次后硬发（不阻塞、吞吐降低；根因属驱动选型，见 `todo.md` 驱动 A/B）② `failsafe_policy.cpp:33` 注释仍提「CDC1」（纯注释陈旧）③ 上板才能定：模块是晶振还是 TCXO（`tcxoVoltage=0.0f` 是晶振假设）、DIO1 边沿、实际灵敏度 |

> 定位提醒（用户强调）：本项目是个人低成本开源项目，审查目的是减少上板调试时固件上的坑。
> CI、单测基建、lint 门禁、RADIOSTATUS、三层递进产测、启动脚本化、日志落 flash、版本号自动生成 都在 `ROADMAP.md` 里等触发条件；
> 分级日志与应用层发送封装收敛明确推后到第一轮上板之后（前者要等真调试经验告诉我们该看什么，后者动的是发送主路径、必须能当场回归）。

| **R4 查官方文档 → 两条硬约束落地**（LB1 / SF1 / CMT1） | 见提交 | 起因：用户纠正「技术事实要查官方文档，不要凭记忆」（此前「LLCC68 不支持 CAD」是我凭记忆下的错判）。① 留档 `docs/references/LLCC68_E220-900M22S.md`（Semtech 数据手册 + 亿佰特规格书 + Zephyr binding + RadioLib 源码/issue #1329，全部附出处；PDF 原件存同目录并 gitignore）；② 驱动类 `SX1268` → `SX1262`（RadioLib 的 `begin()` 按「芯片自报型号」逐字符校验：`SX1268` 类要 「SX1268」，而 E220 实测自报 "SX1261" → 会返回 -2 起不来；`SX1262` 类要的正是 "SX1261"）；③ `ADR_SF_SLOW` 10 → 9（LLCC68 硬约束：BW125 最高 SF9，而开机就停这一档，两机最小验证会哑）；④ 新增 `nvs_cfg_pair_ok()` 联动校验（CLI `nvs_cfg_set` / 网页 `/set` / 整表 `nvs_cfg_save` 三处咽喉全拦，`nvs_cfg_load` 对老配置降档 + 告警，网页/CLI 给明确报错）；⑤ 注释与文档全量同步（SF10→SF9、SX1268→SX1262）；CAD 保留（手册 §6.1.5 确认支持，此前判断作废） | `pio run` SUCCESS（RAM 17.4% / Flash 55.9%，+704B 即校验代码）；三个任务 diff 逐行审：除 `SX1262`/`ADR_SF_SLOW=9` 两处预期改动外全是注释；构建产物、忽略规则、PDF 归档均核对 | 上板待验：① 驱动类能否过型号校验（若仍 -2 按 `SX1262`→`LLCC68`→`SX1268` 阶梯换）② SF9 慢速窗两台机能否互通（`ADR 对齐=是` 应在 5s 内出现）③ TX 功率档位读数是否随设置变化（SX1262 类用 SX1262 功率表，芯片是 LLCC68，可能不吻合 —— 并入 B-1 OCP 测试观察） |

| **R5 智谱 GLM-5.3 独立复审**（换模型交叉审查） | `85d2ee8` + 本提交 | 通读总架构 + 14 个主通路源文件逐行 + vendored RadioLib 关键函数（`transmit`/`finishTransmit`/`startTransmit`/`startReceiveCommon`/`startChannelScan`）+ 从零编译复验；产出根目录 `架构评价.md`（总评 8.4/10）与 `智谱介入审查漏洞.md` | 无致命漏洞。新发现 Z1：`stats` 命令输出进 `Serial1` 黑洞（P1-6 单口分流改造漏改的最后一处）→ 已修（改走 `usb_cli_write`，打印格式与内容一字未改）。并纠正 GLM 的一条建议：其「直接删 `stats` 命令」的前提（`status` 已覆盖全部信息）不成立 —— 核出 `fwd`/`dup`(去重丢弃)/`reasm_to`(重组超时) 三项 `status` 并未覆盖，删命令会丢信息，故改为修出口 | 待上板（已进 `todo.md` §7 并附判据）：Z2「发完能否收」（TX 完成边沿→DIO1→ISR→重挂接收这条链；2 行保险修法已备好）、Z3 切档瞬间的 ToA 精度；顺手项（不单开提交，下次动到该文件再带）：Z4 `mac_send` 返回值区分 LBT 状态、Z7 `failsafe_policy.cpp:33` 陈旧注释；保持不动：Z5/Z6/Z8/Z9；后续可整理：把 `fwd`/`dup`/`reasm_to` 并进 `status` 后即可删掉 `stats` 命令 |

| **F20 WiFi 诊断：新增 CLI `wifiscan` + 187 板 WiFi 故障定位** | 见提交 | 现象：187 板手动 `wifi` 后软件报「AP 已开」，但手机和电脑都扫不到；同一份固件的 237 板正常（本机扫描命中 `EMlink-237`，-92~-95 dBm）。新增 CLI `wifiscan`：必要时 `ota_stop` → `radio_suspend()` → `WiFi.scanNetworks()` → 打印前 10 个 `SSID / rssi / ch` → 关 WiFi → `radio_resume()`；`cli.md` 同步 | 187 实测扫到 27 个 AP（-57 ~ -79 dBm，其中含 `EMlink-237` -59 dBm）→ 2.4G 接收正常，只有发射出不去；`[ota]` 日志对比：237 板打印 `[ota] AP EMlink-237 @ 192.168.4.1`，187 板没有（但被 `#led` 刷屏挤掉，不能据此判失败） | 187 属发射链路故障（陶瓷贴片天线虚焊 / PA / 匹配网络），先补焊天线，无效则芯片级只能换板；`get wifi_ssid` / `get wifi_pass` 返回「未知键」（`cfg_t` 字段未注册进 NVS 键表），SSID 恒为 `EMlink-<node_id>` |

| **F21–F23 诊断探针链 +「空口不通」误判修正** | 见提交 | 起因：EM Studio 发 MAVLink 帧对端收不到（用户报「自定义文本还原不了」）。为定位断点连做三轮固件探针（默认行为一律不变）：F21 新增 `usb_fwd` 配置项（AIR 角色也能把 USB 收到的帧转发到空中，默认 0）+ mesh 三级计数（发/收/投递）；F22 给 `mac_send()` 每个退出点加分项计数（调用/完成/发射错/限流/占空等待/LBT硬发/ACK成/ACK失/重试）；F23 把射频层读包数 `radio_rx_pkt_count()` 与 mesh 总收帧 `mesh_rx_count()` 暴露进 `status`（新增 `收包: 射频=N mesh=N DATA=N` 行）。另：实测确认 `sysid` 写入 NVS 正常生效（`set sysid 200` → save → reset → `get` 与 `status` 均为 200），此前记录的「写 NVS 不生效」在本版固件复现不出 | 探针实测（两块板插同一台电脑的相邻 USB 口）：发送端 `MAC 调用=2 完成=1 发射错=0`、`ACK失=12 重试=12`；接收端 `收包: 射频=0 mesh=0 DATA=0`；`ping` 0/3 超时；降功率到 10 dBm 无效 | 结论修正（重要）：用户随后在 EM Studio 本体上跑通同一链路 → 帧本身是通的、程序也是好的；此前判定的「数据帧双向不通」实为我的自动化脚本测法有问题（同一板子布置下 GUI 能通、脚本不通）：脚本每次都同时打开两个串口、同时 DTR 复位两块板，两板并发重启/初始化射频可能互相干扰；而 EM Studio 是用户分别连接、时间错开。对照历史：15 m 充电宝供电时也稳定通（rssi −98）。教训：台架空口验证不要用「同时复位两块板」的脚本，改用 EM Studio 本体，或错开时间逐个连接。 三轮探针全部保留（不改默认行为），以后再遇「发了但收不到」可直接读 `MAC:` 与 `收包:` 两行定位 |

