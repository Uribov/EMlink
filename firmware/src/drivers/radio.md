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
`radio_init / radio_transmit / radio_startReceive / radio_readPacket / radio_channelActivity / radio_hardReset`

## 验收（P0）
- 两块板 `radio_init` 返回 true、无异常码
- 10m 内互发互收，`rssi`/`snr` 读出有效值并打日志
