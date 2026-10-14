# EMlink 代码审查日志 · 产品交付级差距评估

| 项 | 值 |
|---|---|
| 审查日期 | 2026-10-14 |
| 审查对象 | EMlink @ `94dc848`（工作区干净） |
| 审查范围 | `firmware/src` 全部 21 个模块（4029 行 C++）+ 21 个模块 `.md` + 总架构文档 + 构建配置；并交叉核对了**实际工具链的源码行为** |
| 实测环境 | platform-espressif32 **7.1.3** → Arduino core **2.0.17** → ESP-IDF **4.4.7**；RadioLib **6.6.0**（本地化） |
| 实测编译 | `pio run -e esp32s3-supermini-e220-900m22s` → **SUCCESS**，RAM 17.1%（55916/327680 B）、Flash 55.2%（868753/1572864 B） |
| 审查方法 | 通读全部业务代码；再下沉到框架/库源码验证关键假设（看门狗行为、OTA 回滚语义、USB CDC 数量、CAD 时序、ToA 计算、驱动选型） |

> 框架源码本地路径：`~/.platformio/packages/framework-arduinoespressif32/`（下称 **core**）、`firmware/lib/radiolib/`（下称 **radiolib**）。
> 分级：**P0** = 上板即失败或必现严重故障；**P1** = 功能/可靠性未达交付级（必须修）；**P2** = 工程化/产品化补齐（交付体系）。

---

## 1. 结论摘要

**骨架是好的**（分层清晰、模块自包含、文档先行、编译干净、资源占用健康），但**离"产品交付级"还差三层**：

| 层 | 还差什么 | 对应条目 |
|---|---|---|
| ① 上板能不能活 | 4 个必现缺陷，其中 1 个会导致 **5 秒重启循环** | P0-1 ~ P0-4 |
| ② 能不能真当数传用 | 可靠性机制（ACK/QoS）**根本没接线**；时延/占空比算错 5~28 倍；ADR 在多跳拓扑下失效；加密设计有三处硬伤；OTA 无鉴权且回滚未确认；CLI 实际够不着 | P1-1 ~ P1-12 |
| ③ 能不能叫"产品" | 无 CI、无测试、无版本、无日志系统、无出厂流程、无链路状态上报 | P2 全表 |

**修复不需要推倒架构**：整改建议都以"新增服务层模块（radio_server / transport / ratelimit / logsvc）"落地，保留现有分层与文档结构。建议按 §7 的 M0 → M1 → M2 走。

---

## 2. P0：上板即失败 / 必现严重故障

### P0-1 看门狗接错人 → 开机 5 秒复位循环 ⚠️ 最高优先级

**证据链**（五条，逐层核到框架源码）：

1. 固件把 **loopTask** 注册进任务看门狗：`firmware/src/service/watchdog.cpp:14-17`
   ```cpp
   esp_task_wdt_init(5, true);   // 5s 超时，超时 panic
   esp_task_wdt_add(nullptr);    // setup() 里执行 → 当前任务是 loopTask
   ```
2. 但框架的 loopTask **只在 `loopTaskWDTEnabled == true` 时才喂狗**：`core/cores/esp32/main.cpp:43-49`
   ```cpp
   for(;;) {
     if(loopTaskWDTEnabled){ esp_task_wdt_reset(); }
     loop(); ...
   }
   ```
3. 该标志在 `app_main()` 里被写死 `false`（`core/cores/esp32/main.cpp:69`），**只有调 `enableLoopWDT()` 才会变 true**（`core/cores/esp32/esp32-hal-misc.c:88-95` → `esp_task_wdt_add(loopTaskHandle)`）；**全工程没有 `enableLoopWDT` 调用**（已检索）。
4. 真正调"喂狗"的是 `stats_task`：`firmware/src/app/stats.cpp:88 → watchdog_feed()`，但 IDF 4.4 头文件明写：**`esp_task_wdt_reset()` 只对"当前任务已订阅"时有效，否则返回 `ESP_ERR_NOT_FOUND`**（`core/tools/sdk/esp32s3/include/esp_system/include/esp_task_wdt.h:99-105`）——stats_task 没订阅，喂不到。
5. 触发后果由 sdkconfig 决定：`CONFIG_ESP_TASK_WDT=y` / `PANIC=y` / `TIMEOUT_S=5`（`core/tools/sdk/esp32s3/sdkconfig:1197-1199`）→ **超时直接 panic 重启**。

**影响**：上电约 5 秒后复位，无限循环；所有外场测试都会被这个假象污染。

**修法（二选一）**：
- 方案 A（推荐）：**把 loopTask 从 WDT 移除**，改为让真正的关键任务自己订阅 + 自己喂（rf / mac / mesh / mavlink 各自 `esp_task_wdt_add(nullptr)` 并在循环里喂；stats 只做"总闸"）。
- 方案 B（最小改动）：保留现在的写法，但补 `enableLoopWDT()`（让 loopTask 自己喂），并明确"loop() 挂死也能复位"。

**验收**：上板连续运行 ≥72h 无意外复位；重启计数（`esp_reset_reason()`）里无 WDT 记录。

---

### P0-2 射频不是"单所有者"——4 条执行路径并发操作同一个 radio（SPI 竞态）

总架构 §3.5 的"跨核共享状态"表里**没有 radio 这一行**，但实际有 4 个入口在并发碰它：

| 入口 | 所在任务 | 代码位置 |
|---|---|---|
| 读包 / `startReceive` | `rf_task`（core0，prio 22） | `drivers/radio.cpp:159-179` |
| `transmit` / CAD / `set_sf` | `mac_task`（core0，prio 20） | `drivers/radio.cpp:58-136` |
| **ACK 直接发**（绕过 mac 队列） | `mesh_task`（core0，prio 18） | `net/mesh.cpp:358,364 → link/mac.cpp:108-121` |
| `suspend` / `resume`（**core1**） | `ota_task` | `service/ota.cpp:248,258` |

RadioLib 与底层 SPI **都不是线程安全的**。真实的交错场景举例：mac_task 在"等 ACK 的 5ms 轮询"里让出 CPU（`mac.cpp:80-82`）→ mesh_task 正在发 ACK（SF7 下约 51ms 空中时间）→ mac_task 的延时到期，以更高优先级**抢进来**又开始一轮 SPI 事务 → 两条任务同时驱动同一个 SPI 外设。最坏结果：读回错数据 / 状态机错乱 / 挂死。同类窗口还有 `adr_tick() → radio_set_sf()`（切 SF）与 rf_task 收包之间。

**修法**：
- 最低成本：给所有 `radio_*` 加一把互斥（`SemaphoreHandle_t`），并规定"持锁期间不做长阻塞"。
- 架构正解：**射频单所有者**——所有射频操作变成消息投给 `rf_task`（radio server），ACK 也改走 mac 队列（用一个"立即发送"标记）。
- 无论选哪种，**把 radio 补进 §3.5 共享资源表**，并加一句约定："任何跨任务访问的硬件外设必须有所有者"。

**验收**：压力测试（两节点持续互发 + 同时 ping/ACK + 进出 WiFi 维护模式）跑 30 分钟无 SPI 异常/无挂死。

---

### P0-3 LBT（先听后说）实际是空操作——CAD 结果读早了

`drivers/radio.cpp:107-119`：
```cpp
int16_t st = g_radio->startChannelScan();     // 只是“启动” CAD，命令发完就返回
int16_t res = g_radio->getChannelScanResult(); // 立刻读 IRQ 状态 —— CAD 还没跑完
```
radiolib 的实现证实：`startChannelScan()` 只把芯片设为 CAD 模式就返回（`radiolib/src/modules/SX126x/SX126x.cpp:738-766`），CAD 完成标志要等芯片产生 `CAD_DONE`（SF7/BW125 约 2~3 个符号 ≈ 2~4ms，SF10 约 20~40ms）。立刻去读 → 两个标志都没置位 → 返回 `RADIOLIB_ERR_UNKNOWN`（负数）。

而 `link/mac.cpp:41-42` 把 **`act <= 0` 一律当"信道空闲"** → 直接发送。**结论：LBT 几乎永远返回"空闲"，防撞机制形同虚设**（且这是"越忙越不防"的哑火形态）。

**修法**：轮询 IRQ 状态直到 `CAD_DONE` 或 `CAD_DETECTED`（或改成中断驱动），带超时；映射规则：`DETECTED`→忙、`DONE`→空闲、**超时/未知→按忙处理**（退避后重试，几次后才降级为硬发）。退避时间也应与 ToA 挂钩（见 P1-2）。

**验收**：两台节点同时持续发帧，`dropped_crc` 与重传次数显著下降；抓包（旁路接收）可见碰撞减少。

---

### P0-4 配置写入零校验 + node_id 无唯一化——现场能把自己配死

**(a) CLI `set` 与远程 `cfg` 完全不校验**：`service/nvs_cfg.cpp:115-158` 直接 `atoi/atof` 写 NVS。`set freq 0`、`set sf 99`、`set ttl 0` 都会原样落盘，**重启后射频起不来**。远程路径 `nvs_cfg_apply_kv()`（`nvs_cfg.cpp:207-226`）只拉黑了 `role` 一个键，其余全放行。

**(b) 网页校验与芯片能力不一致**：`service/ota.cpp:163-166` 放行 `power ∈ [-9, 30]`，但 SX1268 驱动的硬性范围是 **-9..22**（`radiolib/src/modules/SX126x/SX1268.cpp:117-123`，越界返回 `RADIOLIB_ERR_INVALID_OUTPUT_POWER`）→ `radio_init()` 失败 → `main.cpp:74-77` 的"重试一次"也失败后，**程序照常继续启动** → 得到一台"没有射频、也没有任何告警"的节点（只有 LED/日志能靠人眼看出来）。

**(c) node_id 没有唯一化**：`net/mesh.cpp:25` 默认 `g_my_node_id = 1`；NVS 里 `node_id=0` 表示"自动分配"但**没有实现自动分配**；两台默认配置的节点**同时是 1 号** → 去重会把对方的帧当作自己的重复帧丢掉、反向路径互相污染，表现是"时通时不通"，非常难查。另外**没有任何撞号检测**。

**修法**：
1. 校验集中到 `nvs_cfg_set()` 一个入口（CLI / 远程 / 网页共用同一套范围），power 上限改 22，freq 视地区白名单（见 P2 合规行）；
2. node_id：首次上电从芯片 MAC 派生一个 1..254 的随机值并落盘；在收到"与自己同号"的帧时计数并告警（CLI/网页可见）；
3. 加"恢复出厂"命令与网页按钮（`nvs_cfg_factory()` 目前只在 NVS 打不开时被调用）；
4. `main.cpp` 射频初始化失败要有明确的可视状态与周期重试，不要静默继续。

**验收**：任意乱输配置（CLI/远程/网页）都无法产生"重启后射频不可用"；两台新板开箱不会撞号。

---

## 3. P1：交付级功能/可靠性差距

### P1-1 ACK/重传机制在业务通路上根本没启用（文档 §8.1 的 QoS 表是空头支票）

- 全代码检索：`ack_req=true` **只出现在 `mesh_ping()`**（`net/mesh.cpp:183`）。`service/mav_router.cpp:189` 把 MAVLink 帧统一设为 `心跳→QOS2 / 其余→QOS0`，投递时**一律 `ack_req=false`**（`:216, :230, :235`）。→ **命令、参数、航点上传没有任何确认与重传**，丢了就丢（架构里 QOS1「ACK+重传×3」、QOS2「逐件确认」未实现）。
- 即便将来打开，ACK 的语义也只是**逐跳**：第一跳中继就回 ACK，中继挂了发送方永远不知道；`link/mac.cpp:31-36` 的 `mac_on_ack_rx()` **只比对 seq、不比对是谁回的**，任何节点（甚至伪造/重放）的 ACK 都能"确认"成功。
- **ACK 帧既不加密也不认证**，且 `net/mesh.cpp:363-365` 在**解密/认证之前**就回 ACK —— 认证失败的坏帧也照样被"确认"。
- 分片补发（NACK）同理：`ack_req=false`（`frag.cpp:128`、`mav_router.cpp:135`）。

**改进方向**：在 `mav_router` 与 `mesh` 之间新增 `transport` 服务模块（端到端 ACK/NACK、序号滑动窗口、去重、重传策略、超时按 ToA 推导），QoS 分级变成它的策略参数；逐跳 ACK 只保留为信道级优化。

**验收**：人为丢包 10%（用 CAD/干扰模拟）下，QOS1 命令送达率 ≥99%、端到端时延 P95 ≤1s；断链/恢复场景可观测。

---

### P1-2 5×~28× 的时延错算：ACK 超时、占空比、TX 超时全都不成立

用 Semtech 官方 ToA 公式（BW125 / CR4/5 / 前导 8 / 显式头 / CRC 开）实测计算：

| 帧型 | SF7 | SF10 | 代码估算（`mac.cpp:53`） | 误差 |
|---|---|---|---|---|
| 16B（ACK/信标） | 51.5 ms | 329.7 ms | 9.8 ms | ×5.3 / ×33.6 |
| 50B | 97.5 ms | 616.4 ms | 20.0 ms | ×4.9 / ×30.8 |
| 100B | 174.3 ms | 1026.0 ms | 35.0 ms | ×5.0 / ×29.3 |
| 179B（分片满帧） | 287.0 ms | 1640.4 ms | 58.7 ms | ×4.9 / ×27.9 |
| 195B（整帧上限） | 312.6 ms | 1804.3 ms | 63.5 ms | ×4.9 / ×28.4 |

后果：
1. **占空比限流器按错 5~28 倍算账**（`link/duty.cpp:10-19` + `mac.cpp:52-56`）：预算 100ms/s 下，实际 SF7 就 31%、**SF10 达 180%** —— 既不合法，也让它形同虚设。
2. **ACK 超时 300ms 在 SF10 下必然超时**（ACK 自身就要 330ms 空中时间 + 接收方处理）：ADR 每周期有 3 个窗口处于 SF10（§13.3），这些窗口里的重传会全部失败并白烧空口；ADR 的 SF10 慢速窗只有 1s，**一个 179B 分片（1640ms）根本塞不进去**。
3. `radio_transmit(data, len, timeout_ms)` 的第三个参数**在 RadioLib 6.6 里是 `addr` 不是超时**（`radiolib/src/protocols/PhysicalLayer/PhysicalLayer.h:101`、`SX126x.h:509`），传入的 `1000` 被静默忽略——把"超时保护"留给了库内部的 `5×ToA`（`SX126x.cpp:242-257`）。调用方以为有 1s 上限，实际是 5×ToA（SF7 满帧 ≈1.5s，SF10 满帧 ≈9s）。

**修法**：写一个 `airtime_ms(len, sf, bw)` 工具函数，**ACK 超时 / 占空比预算 / 发送超时全部由它推导**；`radio_transmit` 的第三个参数改成明确的语义（要么删掉，要么文档写清"库自管超时"）。

---

### P1-3 ADR 的时间模型在真实拓扑下会失效

1. **BEACON 的 TTL=1 且从不转发**（`net/mesh.cpp:292, 349-354`，收到后 `continue`）→ **只有 GCS 直连的节点能收到信标**。而本项目的主场景恰恰是"2+n 多跳"：超出 GCS 一跳的节点**永远停在 SF10**（`adr.cpp:79-85` 的"傻等"分支），而其余节点 70% 时间在 SF7 窗 —— 这些节点**既听不到也传不进去**（它们的帧落在别人的 SF7 窗 = 丢；别人的帧落在它的 SF10 = 它也收不到）。等于半瘫。
2. **无任何失步兜底**：收到一次信标后永久 `g_has_beacon=true`，之后靠本地 `millis()` 自由运行。晶振 ±10~20ppm ≈ 每小时 40~70ms、一天 >1s 漂移；GCS 断电/换机后没人重新对齐，也没有告警。
3. **多 GCS 互相抢锚点**：`adr.cpp:26-30` 收到任意 BEACON 就重置周期起点 → 两个 GCS 的信标交错到达 → 全网周期被反复重置（相位抖动）。
4. **对齐基准本身有偏差**：GCS 的 `g_epoch_ms` 是在**把信标丢进发送队列**时打的（`adr.cpp:54-65` → `mesh_send_beacon()` 只是入队），实际发射时刻 = 入队 + 排队延迟 + 330ms 空中时间；节点侧的对齐点是"**收完**信标"。两侧天然错开 ≈ `330ms(信标 ToA) + 排队延迟`，而 guard 窗只有 500ms —— 空口一忙就会吃光余量。
5. `adr_tick()` 挂在 **mac_task** 循环里（`mac.cpp:134`），而 `adr.h:25` 的注释写"由 stats_task 1s 调一次"——**文档与实现不一致**；且 mac_send 阻塞（等 ACK/等占空比）期间 tick 不跑，切 SF 会迟到。

**改进方向**：① 允许 BEACON 中继转发（ttl>1），或让"听得见锚点的节点"按规则再广播；② 对齐加迟滞 + "连续 N 个周期收不到锚点 → 进入自由运行并告警"；③ 多锚点做主从仲裁（只认 RSSI/节点号最优的锚点）；④ tick 移到独立定时任务；⑤ （可选）信标带上"发射完成时间戳"，节点做单向延迟补偿。

---

### P1-4 加密设计有三处硬伤

`service/crypto.cpp:35-42` 的 nonce = 固定 0(10B) + `src_id` + **16 位 seq**：

1. **Nonce 复用是必然的（最严重）**：每节点 seq 16 位回绕，**发满 65536 帧后 nonce 必然重复**（按 1~5 帧/秒，几小时到几天就会到）。AES-CCM 在 nonce 复用下会泄露明文（两次密文异或即可消掉密钥流）、可伪造标签 —— 这是密码学层面的"必须修"，不是可选项。
2. **AAD 为空**（`crypto.cpp:63-66` 显式传 `nullptr, 0`）：帧头**完全没有被认证**。攻击者不改动标签就能：把 `dst_id` 改成别的节点（密文被导流）、把 `type` 从 DATA 改成 ACK（静默吞掉）、把 `ttl` 清零、清掉 `enc` 标志（让接收端按明文当垃圾处理）。
3. **没有重放防护**：去重窗口只挡 30 秒内的"同 seq 重复"（`mesh.cpp:11, 73`），30 秒后重放旧帧会被当新帧处理。**重放一条旧的 `FRAME_CFG` 就能改写全网参数**（例如把频率改到别的频点）。另外密钥是全网静态共享、明文存 NVS、无轮换、无配对流程。

**改进方向**：① nonce = `src_id ‖ epoch（开机随机 + 持久计数器） ‖ seq`，保证永不复用；② 把帧头（至少 src/dst/type/flags/seq）放进 **AAD**；③ 收端维护"最低接受序号"重放窗；④ 密钥管理（配对/轮换/每设备密钥）列入 M2。

---

### P1-5 OTA 与维护模式：无鉴权、回滚未确认、没有安全阀

1. **AP 默认无密码**（`ota.cpp:270-276` 的 `softAP(ssid)` 分支）且 `/set`、`/crypto`、`/update` **全部无鉴权**：WiFi 范围内任何人都能改密钥、改参数、刷任意固件。**没有固件签名、没有版本校验、没有防降级**。
2. **回滚没确认**：IDF 已配 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（`core/tools/sdk/esp32s3/sdkconfig:68`）→ 新镜像以 **PENDING_VERIFY** 状态启动，应用必须调 `esp_ota_mark_app_valid_cancel_rollback()` 确认；**全工程没有这个调用**（已检索）。两种坏结果二选一：(a) 不确认就重启 → bootloader 回滚到旧固件（OTA"白刷"）；(b) 头文件明说：处于 PENDING_VERIFY 时再 OTA 会返回 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`（`core/tools/sdk/esp32s3/include/esp_ota_ops.h:73-75, 90`）。
3. **维护模式没有安全阀**：`ota_stop()` 只由 CLI/网页的 exit 触发（`ota.cpp:296-299`），**没有超时自动退出**，也没有"飞行中禁止进入"的判断。空中误触 = `radio_suspend()` = 直接失联，而控制权已经不在飞控手里（飞控只能按心跳超时行事）。

**改进方向**：① 维护模式强制密码（或一次性 PIN）且**只在 USB 已枚举时可用**；② 刷写入口加固件签名/版本比较；③ 启动成功后调 `esp_ota_mark_app_valid_cancel_rollback()`（并做"自检通过再确认"的简单状态机）；④ 维护模式加"无操作 N 分钟自动退出 + 低电量/空中状态禁止进入"。

---

### P1-6 CLI 实际上够不着——"双 CDC"在这个框架配置下不存在

README 说"USB CDC 的**第二个**虚拟串口（CLI）"，但实测框架行为是：

- `drivers/usb_cdc.cpp:21-23` 把 CLI 放在 `Serial1`，而 `Serial1` 是**硬件 UART1**（`core/cores/esp32/HardwareSerial.cpp:42`），不是 USB 虚拟串口；
- build_flags 用 `ARDUINO_USB_MODE=1`（S3 内置 USB-Serial/JTAG 硬 CDC）→ 框架把 `Serial` 声明为**唯一那个 USB 口**（`core/cores/esp32/HWCDC.h:110-112`：`extern HWCDC Serial;`），sdkconfig 里也只有一套 CDC 配置（`sdkconfig:285-288`）——**整板只有一个 USB 串口**。

连带问题：
- `stats_print()` 打印到 `Serial1`（`app/stats.cpp:63`）→ **`stats` 命令在界面上永远看不到输出**（写进了一个没接 USB 的 UART）。
- GCS 角色下，唯一 USB 口同时承载 MAVLink 数据与启动日志/OTA 错误输出（`main.cpp:62-64`、`ota.cpp:206-208` 用 `Serial`）→ 文本会混进 QGC 的数据流。
- 配置入口实际只剩：维护模式网页 / 自己焊线引出 UART1（默认引脚 GPIO17/18）。

**改进方向（需拍板）**：① 单口复用：CLI 与 MAVLink 共用一个 USB 口，按"文本/二进制"分流（例如行首 `#` 或 100ms 空闲后进入 CLI 行模式）；② 维护模式网页承担全部配置职能，CLI 降级为 UART1 调试口；③ 改 TinyUSB（`ARDUINO_USB_MODE=0` + 双 CDC）——需要改框架配置并自行接管第二个 CDC 对象，工作量最大。

---

### P1-7 `sysid` 配置项没接线（多机区分不了）

`app/role.cpp:12-19` 里 AIR 的本机 sysid **写死 `MAV_SYSID_FC`(=1)**、GCS 写死 255；NVS 里的 `sysid`（CLI 能设、网页能看、文档 §9 写着"AIR=飞控号"）**在整个 `src/` 里除了读写 NVS 和启动打印外没有任何用途**（已逐文件检索）。
→ 两架飞机的 AIR 节点都是 sysid=1，**QGC 里两台飞机会被当成同一台**；多机场景（§7）无法成立。

**修法**：`role_apply()` 改为"读 NVS `sysid`，为 0 时才用角色默认值"；`sysid` 冲突检测（收到与自己同 sysid 的 HEARTBEAT 时告警）。

---

### P1-8 "中继无需密钥"只在单播时成立；无密钥节点会吞掉加密广播

`net/mesh.cpp:371-383`：广播加密帧到达时**每个节点都会尝试解密，认证失败就 `continue`（不转发）**。因此：
- **没有密钥的中继会静默丢弃且不转发全网广播**（与 README/§13.1 宣称的"中继只转发密文，无需持密钥"矛盾——单播路径确实不需要，广播路径需要）；
- 有密钥但标签校验失败的中继同样不转发（对中继来说，**转发不需要通过认证**，应该照样转发）。
- 顺带：认证失败也计入 `dropped_crc`（统计名不副实），且 ACK 在认证之前就发（见 P1-1）。

**修法**：投递与转发解耦——"认证/解密失败只影响本机投递，不影响转发"（转发路径不碰密钥）。

---

### P1-9 容量上限靠"静默退化"，没有淘汰与可见性

| 结构 | 现状 | 风险 |
|---|---|---|
| 去重表 | 仅 16 个源；**表满就不再去重**（`mesh.cpp:58-70` 返回 nullptr → `dedup_check` 当作新帧） | 重复帧被转发 → 广播风暴放大 |
| 路由表 | 16 条、**只增不淘汰**（`route.cpp:37-43`） | 移动网络中陈旧条目占满 → 新路径学不进来 |
| `addr_map` | 32 条、`last_seen_ms` 写了但**从不用于老化**（`addr_map.cpp:28-33`） | 同上 |
| 分片重组 | 4 并发 × 8 片（够用），但重组键 `(orig_sysid, frag_seq)` **不含 src_id**（`frag.cpp:79`） | 依赖"全网 sysid 唯一"这个**没有任何强制**的前提；两台同 sysid 飞机的分片会拼到一起 |
| 分片补发缓存 | **只有一份**（`mav_router.cpp:39-44`） | 连续两条大消息，前一条的 NACK 补不回来 |
| 大包/补发路径 | 全部走**广播**（`mav_router.cpp:216,235`；`frag.cpp:128`） | 一次航点上传按节点数放大空口开销 |

**修法**：全部表加 LRU/老化淘汰 + 容量超限告警；重组键加 `src_id`；补发缓存改多槽（按 `(sysid, frag_seq)` 索引）；分片走已学到的单播路径（学到就单播，未知才广播）。

---

### P1-10 没有流量整形 → 遥测会把控制流量挤下空口；丢包无计数

- QGC 默认会请求 10~50Hz 遥测流；链路没有按消息类型限速/丢弃策略。`mac_send()` 在占空比超限时是**原地死等**（`mac.cpp:52-56` 的 `while(!duty_reserve) vTaskDelay(10ms)`），期间**心跳/命令全部排队**，同时把 `adr_tick()` 也堵住（P1-3 第 5 条）。极端情况下 1 秒级的等待会把心跳节拍拉长到 1s+，逼近飞控的失联判定。
- 队列满时全部**静默丢**且**没有任何计数**：`radio.cpp:173-175`、`uart_fc.cpp:40`、`usb_cdc.cpp:37`、`mesh.cpp:426`（`rtos_tx_enqueue` 返回值未检查）。产品上无法诊断丢包发生在哪一层。
- `stats_collect()` 的 `link_quality_pct` 恒为 0（`stats.cpp:51`，占位没实现）。

**修法**：按 QoS 类的令牌桶（给控制流预留固定份额，例如 10% 空口时间专供心跳/命令）+ MAVLink 流限速（按消息 ID 的白名单/降频）；每个丢弃点加计数器并上报（见 P2 的 RADIO_STATUS）。

---

### P1-11 射频配置核对项（上板才可最终确认，但代码层面已可改进）

1. **OCP（过流保护）留在 60mA**：RadioLib 的 `begin()` 会把 OCP 设为 60mA（`radiolib/src/modules/SX126x/SX126x.cpp:86`），而 +22dBm 需要约 120~140mA（Semtech 建议 140mA）。固件从未调用 `setCurrentLimit()`。**风险**：高功率发射时触发芯片过流保护 → 输出被限/波形失真（部分批次的模块甚至表现为发射中断）。建议发射功率 >14dBm 时设 `setCurrentLimit(140.0)`，并在空口对比测试里确认。
2. **驱动选型**：工程用 `SX1268` 类驱动 915MHz 模块。但 SX1268 的**镜像抑制校准表只覆盖 779/470/430MHz 三档**（`SX1268.cpp:64-73`），915MHz 会落入"自定义校准，可能有效可能无效"分支（库自己这么注释的）；而 **SX1262 类有 902–928MHz 档**（`SX1262.cpp:63-65`），RadioLib 也有独立的 `LLCC68` 类。建议上板后对比两种驱动的**接收灵敏度/RSSI/丢包率**，选更优者（三个类接口一致，`radio.cpp` 里换一个类名即可）。
3. `radio_init()` 里 `begin(..., preambleLength=8, tcxoVoltage=0.0)` 与硬件（无 TCXO 晶振版）一致 ✔；`setDio2AsRfSwitch` 由库内部调用，但 DIO2 悬空所以无实际影响（文档里"禁止调用"的表述可以放宽为"DIO2 悬空，不依赖它"）。

---

### P1-12 其他实现细节（小，但会影响外场判断）

1. `frag_tick()` 只在 `mesh_task` **收队列超时 100ms** 时才被调用（`mesh.cpp:322-325`）→ 空口持续繁忙时补发机会被饿死。应挂到独立时基。
2. `radio_channelActivity()` 在 `g_suspended` 时返回 -1，而 `mac_lbt` 把负数当"空闲"（P0-3 同源问题）。
3. 看门狗计数无意义：`g_wdt_resets++` 紧接 `esp_restart()`（`watchdog.cpp:26-28`）——重启后归零，永远读不到；`g_rf_fault_count` **从不清零**，注释说"连续 3 次"，实际是"累计 3 次"。
4. LED 实现远小于 `led_states.md` 的规格：只有"每 1s 设一个常亮色"，`LED_HEALTH_IDLE / CONFIG / FAULT / RESET` 从未被传入（`stats.cpp:113-117`），没有闪烁/去抖/上电白闪。
5. 死代码/未接线清单（建议一轮清理）：`q_frag`（建了没人用，白占 1.7KB RAM）、`mac_should_ack()`、`mav_router_handle_msg()`、`ota_handle()`、`addr_map_init()` / `route_init()` / `frag_init()`（从未被调用）、`EV_CFG_CHANGED`（没人监听，`save` 命令实际只打印一行）、`duty_set_max()`、`beacon_s` / `name` / `sysid`（配置项未接线）。
6. `stats`/调试输出目标错误（P1-6）之外，`main.cpp:116-119` 的"未配置回显"在 CDC0 上回显，而 CLI 在 CDC1 —— 开箱引导流程实际是断的。

---

## 4. P2：工程化 / 产品化缺口（交付体系）

| 类别 | 现状 | 建议 |
|---|---|---|
| 版本 | 无版本号、无 CHANGELOG、无 git tag；固件里无构建信息 | `EMLINK_VERSION` + git SHA 编译期注入；`status`/网页可见；打 tag |
| 测试 | **零单元测试**；只有 `design/verify_webui.py` 一个页面校验脚本 | 纯逻辑模块（frame_codec / dedup / frag / route / adr 相位 / duty / 去重窗）做 host 单测（PlatformIO `native` env）；双板 HIL 脚本化 |
| CI | 无 `.github`（已核对），全靠手动编译 | GitHub Actions：多 env 编译 + host 单测 + Flash/RAM 尺寸门禁 |
| 日志 | 只有散落的 `Serial.printf`；文档说的 ERR/WARN/INFO/DBG 四级不存在 | 统一 `log.h`（等级/时间戳/环形缓冲/运行时可导出） |
| 崩溃诊断 | coredump 分区在、IDF 默认已开 to-flash，但**没有取回流程与文档** | README/CLI 补"取 coredump"步骤；开工打印 `esp_reset_reason()` 并持久化重启原因 |
| 地面端可观测 | 不向 QGC 上报任何链路信息 | 周期注入 `RADIO_STATUS`（RSSI/SNR/丢包率）+ 失联/恢复 `STATUSTEXT` |
| 出厂/量产 | 无出厂测试模式、无设备唯一 ID、无 NVS schema 版本/迁移、无恢复出厂命令 | `factory` 命令 + 量产自检（LED/射频回环/NVS/频点）+ MAC 派生 ID + `cfg_version` 迁移 |
| 合规 | 网页放行 400–1000MHz（模块只到 ~930）；占空比实际超 10%；无地区频段表 | 频段白名单按地区（CN 470-510 / US 902-928 / EU 863-870）；占空比按实测 ToA 计 |
| 安全基线 | 无固件签名、无 Flash 加密/安全启动（密钥可被读出）、维护口无鉴权 | M2 规划：至少 OTA 签名 + NVS 加密 |
| 文档一致性 | 存在多处"文档 ≠ 实现"：双 CDC、QoS 表、`beacon_s`、`sysid`、`adr_tick` 归属、"双看门狗"、日志四级、"中继无需密钥"、LED 节奏、`mavlink monitor` 预留 | 按模块 `.md` 做一次对账；可加"符号存在性"检查脚本防再漂移 |

---

## 5. 架构层面的不足与改进方向

1. **共享资源模型不完整（最核心的架构问题）**。§3.5 列了内存变量的锁，却漏了**射频/SPI 这个最大的共享资源**（P0-2）。改进：确立"**外设单所有者**"原则（radio 只被一个任务碰，其余模块通过队列交互），并把这条约定写进总架构。
2. **缺一层"端到端传输"**。现在把"信道级逐跳 ACK"和"用户要的可靠送达"混在一起，导致 QOS1/2 无处落地（P1-1）。改进：新增 `transport` 服务层（端到端 ACK/NACK + 序号窗口 + 重传策略），QoS 分级变成它的参数。
3. **时间模型要有兜底与自愈**。ADR 是"开环全网 TDM"，没有失步检测/补偿/多锚点仲裁（P1-3）。改进：BEACON 可多跳 → 锚点带发射时间戳 → 漂移超限降级到"兼容模式"并告警 → 多 GCS 主从选举。
4. **配置系统要"单入口 + 版本 + 锁"**：校验收进 `nvs_cfg_set()`；加 schema 版本迁移；加"出厂/只读"模式；远程可改键收紧（改完即失联的 `freq`/`sf` 不开放远程广播）。
5. **流量治理成层**：令牌桶 + 按类配额 + MAVLink 限速/丢弃表；`mac_send` 的"死等"改成"预约制"（算好下次可用时刻交给调度器）。
6. **可观测性成层**：`log` 服务 + 统计上报（MAVLink）+ 复位原因/事件黑匣子（NVS 环形记录）；"当前状态"变成 CLI/网页/MAVLink 共读同一份快照。
7. **规模上限显式化**：节点数/去重表/路由表/`addr_map` 都定义容量与淘汰策略，超限要可见（告警），不许静默劣化（P1-9）。

> 落地方式建议：全部以**新增模块**（`radio_server` / `transport` / `ratelimit` / `logsvc`）的形式加入服务层，不动现有分层与命名约定。

---

## 6. 做得好的地方（保留）

- **工程结构与文档纪律**在同类项目里属上游：模块边界、依赖图、启动顺序、并发约定都写清楚了，且代码基本照做了（抽查 portMUX 使用、队列所有者、帧格式字节序 —— 一致）。
- 帧格式设计合理：版本位、QoS 位、`relay_id` 反向路径、`net_id` 预留；CRC16 端到端 + LoRa 硬件 CRC 逐跳双重保护。
- 扩展模块（crypto / adr / ota）与主体同构，没有破坏分层；"WiFi 与 LoRa 二选一"是清醒的取舍。
- 编译干净、依赖全离线（三个库本地化）、资源占用健康（RAM 17.1% / Flash 55.2%）。

---

## 7. 建议修复路线

| 阶段 | 内容 | 出口标准（验收） |
|---|---|---|
| **M0 上板前（阻断项）** | P0-1 看门狗；P0-2 射频单所有者；P0-3 CAD；P0-4 校验+node_id；顺带：P1-6 的 CLI 出口定案、P1-11 的 OCP/驱动核对、附录 C 清单 | 连续 72h 无意外复位；两板互发互收稳定；配置怎么写都炸不了；维护模式进出可控 |
| **M1 交付级功能** | P1-1 传输层；P1-2 ToA 换算；P1-3 ADR 兜底；P1-4 加密修复（nonce/AAD/重放）；P1-5 OTA 确认+鉴权+安全阀；P1-7 sysid；P1-9/P1-10 容量与流量治理 | 通过 §11 三指标 + 断链/重连/多机/多跳/丢包矩阵 |
| **M2 工程化与量产** | P2 全表 + 架构改进 §5.4~5.7 | CI 全绿；版本可追溯；出厂流程文档化；现场可诊断（日志/RADIO_STATUS/coredump） |

**建议把 §11 的三指标扩成矩阵**（每项都有明确阈值与测试方法）：投递率（视距/中继/干扰三档）、端到端时延（P50/P95）、断链→返航时间、重连时间、OTA 成功率与断电中断恢复、多机并发、72h 长跑复位次数 = 0。

---

## 附录 A：Time-on-Air 参考表（BW125 / CR4/5 / 前导 8 / 显式头 / CRC 开）

| 负载(B) | SF7 | SF8 | SF9 | SF10 | SF11 |
|---|---|---|---|---|---|
| 16 | 51 ms | 92 ms | 165 ms | 330 ms | 660 ms |
| 50 | 98 ms | 174 ms | 329 ms | 616 ms | 1315 ms |
| 100 | 174 ms | 329 ms | 596 ms | 1026 ms | 2216 ms |
| 179 | 287 ms | 534 ms | 940 ms | 1640 ms | 3609 ms |
| 195 | 313 ms | 585 ms | 1036 ms | 1804 ms | 3936 ms |

（用于推导 ACK 超时、占空比预算、调度窗口；公式：Semtech AN1200.13。）

## 附录 B：逐条自验证方法（不上板）

```bash
# P0-1 看门狗：确认没有 enableLoopWDT、且喂狗在未订阅的任务里
grep -rn "enableLoopWDT" firmware/src                 # 期望：无输出
grep -rn "esp_task_wdt_add\|esp_task_wdt_reset" firmware/src
#   → 对照 core/cores/esp32/main.cpp:34,47-49（loopTaskWDTEnabled）与 esp_task_wdt.h:99-105

# P0-3 CAD：确认“启动后立刻读结果”
sed -n '107,119p' firmware/src/drivers/radio.cpp
#   → 对照 radiolib/src/modules/SX126x/SX126x.cpp:738-785（startChannelScan 只发命令）

# P1-2 ToA 误差：看 duty 估算公式
sed -n '52,56p' firmware/src/link/mac.cpp             # 5 + len*300/1000

# P1-1 QoS 未接线：确认业务帧 ack_req 全为 false
grep -rn "ack_req" firmware/src --include=*.cpp       # 只有 mesh_ping 是 true

# P1-5 回滚未确认：确认没有 mark_app_valid
grep -rn "mark_app_valid\|esp_ota_mark" firmware/src   # 期望：无输出

# P1-6 CDC：确认 Serial1 是硬件 UART
sed -n '37,45p' core/cores/esp32/HardwareSerial.cpp   # HardwareSerial Serial1(1);
sed -n '110,112p' core/cores/esp32/HWCDC.h            # extern HWCDC Serial;（唯一 USB 口）

# P1-7 sysid：确认配置项没人用
grep -rn "sysid" firmware/src --include=*.cpp --include=*.h | grep -v orig_sysid
```

## 附录 C：上板前核对清单（硬件相关，需实测）

- [ ] **板卡定义 ≠ 实际硬件**：`platformio.ini:20` 的 `board = esp32-s3-devkitc-1` 被 PIO 解析为 "8 MB QD, **No PSRAM**"，而实际是 S3FH4R2（4MB Flash + 2MB PSRAM）。Flash 已用 `board_build.flash_size=4MB` 覆盖 ✔；但 `-DBOARD_HAS_PSRAM` 仍挂着。**建议先删掉该宏**（固件根本不用 PSRAM，全部静态分配），将来要用再按 QSPI 模式正确配置 `memory_type` 并用 `ESP.getPsramSize()` 验证。
- [ ] 射频 OCP：确认 `setCurrentLimit(140.0)` 与发射功率匹配（P1-11）。
- [ ] 驱动选型对比：SX1268 vs SX1262/LLCC68 驱动的接收灵敏度/丢包率（915MHz 镜像校准档位差异，P1-11）。
- [ ] 首次烧录整片擦除（分区表已换，README 已写）。
- [ ] 上电后第一时间串口观察：**若每 ~5s 复位一次 → P0-1 命中**（预期命中）。
- [ ] RSSI/SNR 与距离实测（10m → 500m 阶梯）、双供电红牌（Type-C 与 4Pin 禁同插）。

---

*本日志为只读审查产物，未改动任何代码；后续修复将按 M0 逐项进行并同步各模块 `.md`。*
