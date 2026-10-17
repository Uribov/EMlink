# radio —— 射频驱动抽象接口

## 职责
向上层（mac/mesh）提供统一射频接口：发送 / 接收 / CAD 侦听 / 复位 / 空中时间换算。
上层**不直接碰 RadioLib 或具体芯片**，全部经本接口。
**射频单所有者（P0-2）**：所有芯片操作只在 `rf_task` 上下文执行；上层调用 `radio_*` 时，内部自动变成"请求队列 + 阻塞等结果"，调用方无需感知。

## 多芯片实现方式
- 本文件是「抽象接口」，具体芯片在 `radio.cpp` 里按芯片类型宏分叉：
  - `RADIO_SX126X`（SX1261/2/8 + LLCC68）
  - `RADIO_SX127X`（SX1276/7/8，将来加）
- 芯片类型宏由 `src/radios/<射频>.h` 定义（如 `e220-900m22s.h` 定义 `RADIO_SX126X`）

## 依赖
- RadioLib（`platformio.ini` 已加 `jgromes/RadioLib`）
- `pinmap.h`：SPI + DIO/BUSY/NRST/RXEN/TXEN 引脚（来自 `radios/<射频>.h`）
- `airtime.h`（P1-2，同层）：LoRa 空中时间公式，供 `radio_airtime_ms()` 用；纯整数、无依赖

## 关键实现点（SX126x 分支）
1. 实例化：`Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY)`，再用驱动类包装 —— **本板用 `SX1262` 类**（芯片是 LLCC68，但 RadioLib 的 `begin()` 会拿"芯片自报型号"与类声明比对：`SX1268` 类要 `"SX1268"`、`SX1262` 类要 `"SX1261"`，而 E220 实测自报 `"SX1261"` → 用 `SX1268` 类会返回 -2 起不来。依据：`docs/references/LLCC68_E220-900M22S.md` §3；若仍报 -2 按 `SX1262`→`LLCC68`→`SX1268` 阶梯换）
2. 收发切换：`radio.setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN)`（active-high）
3. **禁止** `setDio2AsRfSwitchCtrl()`（DIO2 悬空）；晶振版不调 `setTCXO`
4. CAD：`radio.startChannelScan()` 启动 → **状态驱动轮询** `getChannelScanResult()` 直到 `CAD_DONE`/`CAD_DETECTED`（超时按 SF/BW 推导：5 符号 + 3ms，SF7≈8ms / SF9≈23ms；P0-3）。**LLCC68 支持 CAD**（数据手册 §6.1.5），LBT 可正常用
5. 中断：DIO1 挂 GPIO 中断，ISR 内只发任务通知给 `rf_task`
6. **OCP 过流保护**（P1-11）：`begin()` 内部会把 OCP 设成 60mA，而 +22dBm 发射需 120~140mA（Semtech 建议 140mA）→ **功率 > 14dBm 时必须在 `begin()` 之后**调 `setCurrentLimit(140.0f)`；两处 `begin()`（`radio_init` 成功路径、`radio_retry_tick` 重试成功分支）都要，否则高功率发射可能触发过流保护（输出被限/波形失真）。≤14dBm 保持库默认 60mA。
   （LLCC68 数据手册 §5：OCP 复位默认就是 140mA、2.5mA 步进 → 我们的 140 与芯片默认一致。）
7. **SF 与带宽必须配对**（LLCC68 硬约束）：**BW125→SF≤9**、BW250→≤10、BW500→≤11。配置面已由 `service/nvs_cfg` 的 `nvs_cfg_pair_ok()` 拦住（CLI/网页/远程 FRAME_CFG 全走它）；ADR 只用 SF7/SF9，三种带宽下都合法。（依据：`docs/references/LLCC68_E220-900M22S.md` §2）

## 对外接口
见 `radio.h`。所有芯片实现同一组函数：
`radio_init / radio_transmit / radio_transmit_prio（高优先发送，ACK 用） / radio_startReceive / radio_readPacket / radio_channelActivity / radio_airtime_ms / radio_hardReset / radio_set_sf / radio_suspend / radio_resume / radio_is_ready / radio_rx_drop_count / radio_req_drop_count`（后两个是 P1-10 丢弃计数）
契约（P0-2）：以上接口内部为"请求队列 + 阻塞等结果"——**调用它们的任务不得把任务通知（task notification）用于其它用途**。

- `radio_transmit(data, len)` **不带超时参数**（P1-2）：RadioLib 的 `transmit` 第三参是 `addr` 不是超时，传超时值会被静默忽略；发送超时由 RadioLib 内部按 5×ToA 自管。
- `radio_airtime_ms(len)` 按**当前实际**调制参数（`g_cur_sf/g_cur_bw_khz/g_cur_cr`）算空中时间；未就绪返回 0（调用方须按"算不出"处理）。

## 验收（P0）
- 两块板 `radio_init` 返回 true、无异常码
- 10m 内互发互收，`rssi`/`snr` 读出有效值并打日志

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_radio` / `g_mod`：RadioLib `SX1262`/`Module` 实例（堆上 new）
- `g_rf_task`：rf_task 句柄
- `g_last_rssi` / `g_last_snr`：最近收包信号
- `g_tx_fail`：连续发送失败计数（自救触发用）
- `g_cur_sf` / `g_cur_bw_khz` / `g_cur_cr`（P1-2）：当前实际调制参数——CAD 超时推导与 `radio_airtime_ms()` 共用；`radio_init` 成功时设定，`radio_retry_tick()` 重试成功时同 `g_init_*` 一并复位（避免重试后参数不一致）
- `g_radio_ready`（P0-4）：就绪状态（init 成功且未被撤下）；**未就绪时所有射频操作短路返回，不碰 SPI**
- `g_init_*`（P0-4）：init 参数快照（freq/bw/sf/cr/sync/pwr），失败后周期重试用它重新 `begin()`

### 单所有者与请求队列（P0-2）
- `g_radio_owner` = `rf_task`；`radio_start()` **先建两个请求队列、再创建任务**，然后把 owner 标为 rf_task（避免启动期 `adr_init` 走"无所有者→直接执行"路径）
- 请求模型（radio.cpp 内 static，不进头文件）：`radio_req_t{op, sf, len, result, waiter, data[FRAME_MTU]}`；队列元素是**指针**——请求结构放在调用者栈上，调用者阻塞到执行完成，生命周期安全
- 公开接口 → `radio_call(op, sf, data, len, hi)`：
  - `owner == nullptr`（启动早期）或 `owner == 自己`（rf_task / 周期重试 / 射频自救路径）→ `radio_exec_inline()` **直接执行**（因此 `watchdog_rf_fault → radio_hardReset` 在所有者内不会自投自收死锁）
  - 否则 → 清残留通知 → 投 `q_radio_hi`（控制操作 / ACK / 控制帧）或 `q_radio_lo`（普通数据帧）→ `xTaskNotifyGive(owner)` 唤醒 → `ulTaskNotifyTake(portMAX_DELAY)` 等结果（WDT 兜底）
- `rf_task` 每轮：喂狗 → `radio_retry_tick()` → **先服务 hi 再服务 lo**（执行完 `xTaskNotifyGive(waiter)` 回结果）→ 无请求则等 DIO1/唤醒（1s 超时）→ 读包投 `q_rf_rx`
- 优先级语义：`q_radio_hi` = CAD / set_sf / suspend / resume / reset + ACK·控制帧 TX；`q_radio_lo` = 普通数据帧 TX → **ACK 不会被排队的数据帧挡住**（mac_task 可能正阻塞在自己等 ACK 的窗口里）
- 有界等待：每个调用者同一时刻只有 1 个在途请求，队列 hi6/lo4；最坏等待 ≈ 其它生产者各自一个操作的时长之和（实测上限远小于 5s WDT）

### 数据流
DIO1 上升沿中断 `radio_isr` → `vTaskNotifyGiveFromISR(g_rf_task)` → rf_task 被唤醒
→ `radio_readPacket()` 读包 → 装 `queue_item_t` → 投 `q_rf_rx`（消费方 mesh_task）

### 关键实现细节
1. SPI 重映射：`SPI.begin(SCK=9, MISO=11, MOSI=10, SS=8)`（GP13=BUSY 占 FSPI 默认 MISO）
2. 无 TCXO：`begin(..., tcxoVoltage=0.0f)`；禁 `setDio2AsRfSwitchCtrl()`（DIO2 悬空）
3. 射频开关 active-high：`setRfSwitchPins(RXEN=1, TXEN=2)`
4. 发送：`transmit(const_cast<uint8_t*>(data), len)`——RadioLib 签名非 const 但不改数据；**第三参是 addr，不传超时**（P1-2）
5. 收包：`readData(buf, maxlen)` 两参 + `getPacketLength()` 取实际长度（RadioLib 6.x）
6. CAD（`radio_channelActivity`，LBT 用）：`startChannelScan()` 只**启动** CAD（非阻塞），随后**轮询** `getChannelScanResult()` 直到 `CAD_DONE`（返回 0=空闲）/`CAD_DETECTED`（返回 1=忙）；超时按 SF/BW 推导（5 符号 + 3ms），超时返回 -2、错误返回负值——**调用方（mac_lbt）把负值一律按"忙"处理**（P0-3；原实现在 CAD 完成前就读结果，导致永远判"空闲"）
7. 运行时切 SF（`radio_set_sf`，ADR 用）：`standby() → setSpreadingFactor() → startReceive()`，**必须先 standby**（SX126x 芯片规定改调制参数不能在收发中途写寄存器）；同时更新 `g_cur_sf`
8. 暂停/恢复（`radio_suspend`/`radio_resume`，OTA 维护模式用）：suspend 设 `g_suspended=true` + standby，之后所有射频操作 no-op；resume 清标志 + startReceive
9. **任务看门狗**（P0-1）：`rf_task` 开头 `watchdog_register_current()` 订阅；等 DIO1 通知带 **1s 超时**（`ulTaskNotifyTake(pdTRUE, 1000ms)`），超时也醒来 `watchdog_feed()`，保证长时间无包时不被 WDT 判死（通知是计数锁存，超时后到达的中断下一轮仍能命中，不丢包）
10. **就绪状态与周期重试**（P0-4 / S4 修订）：`radio_is_ready()` 对外暴露；`radio_init` 失败置 false 并记 `g_last_err`。`rf_task` 每轮调 `radio_retry_tick()`：
    - **从未成功过**（`!g_ever_ready`）→ 视为"没有射频模块"，**一次都不自动重试**，置 `g_absent` 并打印诊断（上板实测：开机那次 `begin()` 正常返回失败，但紧接着的重试可能死等 >5s → rf_task 不喂狗 → 任务看门狗重启）；
    - **曾经成功过**（热故障）→ 每 10s 重试，成功则重挂 DIO1 中断、置就绪并打印；
    - 重试前先 `watchdog_feed()`；重试不触发 `watchdog_rf_fault`（避免被重试放大）
11. **射频单所有者**（P0-2）：可用 `grep -rn "g_radio->" firmware/src` 自查——除 `drivers/radio.cpp` 外全工程再无 `g_radio->` / `SPI.` / `attachInterrupt`；上层模块（adr/mac/ota/watchdog）调用方式零改动。约束：调用 `radio_*` 的任务**不得把任务通知用于其它用途**（通知=请求完成信号）
12. **空中时间（P1-2）**：`radio_airtime_ms(len)` = `airtime_ms(len, g_cur_sf, g_cur_bw_khz, g_cur_cr)`（公式与实测表见 `airtime.md`）。它是**纯计算**，不走请求队列、不碰 SPI，任何任务可随时调；未就绪返回 0

### 故障自救（架构 §8.2）
- `radio_transmit` 连续 3 次失败 → `watchdog_rf_fault()`（软复位 NRST，累计 3 次整机重启）
- `radio_init` 失败 → 记错误码并返回 false；**只有 `g_ever_ready`（曾经成功过）才调 `watchdog_rf_fault()`** —— 否则"没插模块"会被放大成每 33 s 一次的整机重启循环（上板实测过的坑）

### 无射频模块的诊断与降级（S4，2026-10-17 上板实测）

现场：板子没接 E220 时，`adr_init()` 的 `radio_set_sf()` 会投请求给 `rf_task` 并**无限等回复**（原 `portMAX_DELAY`）；而 `rf_task` 正卡在重试的 `begin()` 里 → 两边互等 → `setup()` 停死（CLI/WiFi 都起不来）→ 5 s 后任务看门狗 panic → 每 33 s 重启一次。

现在的行为（三条，缺一不可）：

1. **请求等待有上限**：`radio_call()` 用 `RADIO_REQ_TIMEOUT_MS = 3000` 等所有者，超时返回 -1 并计 `radio_req_timeout_count()`；调用方永不被单个卡死的所有者拖死。
   请求**按值放进队列**（`xQueueCreate(..., sizeof(radio_req_t))`），不是指针 —— 否则超时返回后栈帧销毁、`rf_task` 稍后写 `req->result` 就是 use-after-free（实测 `LoadProhibited`，`addr2line` 定位到 `rf_task_func`）。
2. **无模块即停用**：从未成功过 → `g_absent = true`，之后所有 `radio_*`（除 suspend/resume）立即返回 -1，不排队不等待；重试也不再进入可能死等的 `begin()`。
3. **明确的现场诊断**：启动日志 + CLI `status` 都直说 `模块=未检测到（err=-2）← 检查 E220 排线/供电，装好后 reset`。

诊断接口：`radio_last_error()`（最近错误码）、`radio_absent()`、`radio_init_fail_count()`、`radio_req_timeout_count()`。

已知边界：`begin()` 内部的死等**没有从根上消除**（RadioLib 等 BUSY 无超时），只是靠"无模块不重试"绕开；若开机那一次 `begin()` 就死等，仍会被看门狗重启。超时返回后请求可能稍后仍被执行（例如迟到发一帧），不提供 exactly-once。
