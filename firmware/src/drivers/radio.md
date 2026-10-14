# radio —— 射频驱动抽象接口

## 职责
向上层（mac/mesh）提供统一射频接口：发送 / 接收 / CAD 侦听 / 复位。
上层**不直接碰 RadioLib 或具体芯片**，全部经本接口。

## 多芯片实现方式
- 本文件是「抽象接口」，具体芯片在 `radio.cpp` 里按芯片类型宏分叉：
  - `RADIO_SX126X`（SX1261/2/8 + LLCC68）
  - `RADIO_SX127X`（SX1276/7/8，将来加）
- 芯片类型宏由 `src/radios/<射频>.h` 定义（如 `e220-900m22s.h` 定义 `RADIO_SX126X`）

## 依赖
- RadioLib（`platformio.ini` 已加 `jgromes/RadioLib`）
- `pinmap.h`：SPI + DIO/BUSY/NRST/RXEN/TXEN 引脚（来自 `radios/<射频>.h`）

## 关键实现点（SX126x 分支）
1. 实例化：`SX1268 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY)`
2. 收发切换：`radio.setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN)`（active-high）
3. **禁止** `setDio2AsRfSwitchCtrl()`（DIO2 悬空）；晶振版不调 `setTCXO`
4. CAD：`radio.startChannelScan()` + `radio.getChannelScanResult()` 判忙闲
5. 中断：DIO1 挂 GPIO 中断，ISR 内只发任务通知给 `rf_task`

## 对外接口
见 `radio.h`。所有芯片实现同一组函数：
`radio_init / radio_transmit / radio_startReceive / radio_readPacket / radio_channelActivity / radio_hardReset / radio_set_sf / radio_suspend / radio_resume / radio_is_ready`

## 验收（P0）
- 两块板 `radio_init` 返回 true、无异常码
- 10m 内互发互收，`rssi`/`snr` 读出有效值并打日志

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_radio` / `g_mod`：RadioLib `SX1268`/`Module` 实例（堆上 new）
- `g_rf_task`：rf_task 句柄
- `g_last_rssi` / `g_last_snr`：最近收包信号
- `g_tx_fail`：连续发送失败计数（自救触发用）
- `g_radio_ready`（P0-4）：就绪状态（init 成功且未被撤下）；**未就绪时所有射频操作短路返回，不碰 SPI**
- `g_init_*`（P0-4）：init 参数快照（freq/bw/sf/cr/sync/pwr），失败后周期重试用它重新 `begin()`

### 数据流
DIO1 上升沿中断 `radio_isr` → `vTaskNotifyGiveFromISR(g_rf_task)` → rf_task 被唤醒
→ `radio_readPacket()` 读包 → 装 `queue_item_t` → 投 `q_rf_rx`（消费方 mesh_task）

### 关键实现细节
1. SPI 重映射：`SPI.begin(SCK=9, MISO=11, MOSI=10, SS=8)`（GP13=BUSY 占 FSPI 默认 MISO）
2. 无 TCXO：`begin(..., tcxoVoltage=0.0f)`；禁 `setDio2AsRfSwitchCtrl()`（DIO2 悬空）
3. 射频开关 active-high：`setRfSwitchPins(RXEN=1, TXEN=2)`
4. 发送：`transmit(const_cast<uint8_t*>(data), len)`——RadioLib 签名非 const 但不改数据
5. 收包：`readData(buf, maxlen)` 两参 + `getPacketLength()` 取实际长度（RadioLib 6.x）
6. CAD：`startChannelScan()` + `getChannelScanResult()`（0=空闲 1=忙 负=错误）
7. 运行时切 SF（`radio_set_sf`，ADR 用）：`standby() → setSpreadingFactor() → startReceive()`，**必须先 standby**（SX126x 芯片规定改调制参数不能在收发中途写寄存器）
8. 暂停/恢复（`radio_suspend`/`radio_resume`，OTA 维护模式用）：suspend 设 `g_suspended=true` + standby，之后所有射频操作 no-op；resume 清标志 + startReceive
9. **任务看门狗**（P0-1）：`rf_task` 开头 `watchdog_register_current()` 订阅；等 DIO1 通知带 **1s 超时**（`ulTaskNotifyTake(pdTRUE, 1000ms)`），超时也醒来 `watchdog_feed()`，保证长时间无包时不被 WDT 判死（通知是计数锁存，超时后到达的中断下一轮仍能命中，不丢包）
10. **就绪状态与周期重试**（P0-4）：`radio_is_ready()` 对外暴露；`radio_init` 失败置 false；`rf_task` 每轮调 `radio_retry_tick()`——未就绪且距上次 ≥10s 就重新 `begin()`，成功则重挂 DIO1 中断、置就绪并打印日志。**重试不触发 `watchdog_rf_fault`**（避免"累计 3 次整机重启"被周期重试放大）

### 故障自救（架构 §8.2）
- `radio_transmit` 连续 3 次失败 → `watchdog_rf_fault()`（软复位 NRST，累计 3 次整机重启）
- `radio_init` 失败 → `watchdog_rf_fault()` + 返回 false；main.cpp 里重试一次
