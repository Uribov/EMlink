# 硬件方案（多方案）架构说明

## 理念

一套固件代码库，适配「多种 MCU 板 × 多种射频模块」的组合。
每个组合 = 一个方案，编译出一个独立固件。低成本批量造不同硬件时，只改配置、不改业务代码。

拆分的目的是让业务代码只认一套引脚宏，换硬件时不必翻业务代码。


## 方案 = 板 + 射频（两层拆分）

| 层 | 目录 | 放什么 |
|---|---|---|
| 板级 | `src/boards/<板>.h` | 飞控 UART、LED、悬空脚等「跟哪块 MCU 板有关」的引脚 |
| 射频级 | `src/radios/<射频>.h` | SPI、DIO、BUSY、NRST、RXEN/TXEN 等「跟哪个射频模块有关」的引脚 + 芯片类型宏 |
| 入口 | `src/pinmap.h` | 按编译宏 include 上面两类，业务代码只认它 |

板、射频、入口三层各管一件事，加硬件只动前两层。


## 怎么加一块新板

1. `src/boards/` 新建 `<板名>.h`，定义 `PIN_LED / PIN_FC_TX / PIN_FC_RX / FC_UART_NUM` 等
2. `pinmap.h` 加一个 `#elif defined(BOARD_<板名>)` 分支

## 怎么加一个新射频

1. `src/radios/` 新建 `<射频名>.h`，定义 `PIN_LORA_*` 9 个引脚 + 芯片类型宏（如 `RADIO_SX127X`）
2. 驱动相同（RadioLib 同族）→ 无需写驱动；驱动不同 → 在 `drivers/` 按芯片类型宏写实现，
   对外接口保持 `radio.h` 那套：`radio_init / radio_transmit / radio_startReceive / radio_readPacket / radio_channelActivity / radio_hardReset`
3. `pinmap.h` 加 `#elif defined(RADIO_<射频名>)` 分支

两处都只加头文件和分支，业务代码零改动。


## 怎么加一个新方案（板+射频组合）

`platformio.ini` 加一个 `[env:板名-射频名]`，`build_flags` 里写 `-DBOARD_XXX -DRADIO_XXX`。

## 编译

- 单个方案：`pio run -e <方案名>`
- 全部方案：`bash scripts/build_all.sh`
- 固件输出：`.pio/build/<方案名>/firmware.bin`

## 当前已有方案

| 方案名 | 板 | 射频 | 驱动 |
|---|---|---|---|
| esp32s3-supermini-e220-900m22s | ESP32-S3 SuperMini | E220-900M22S（LLCC68 芯片） | RadioLib `SX1262` 类（型号校验要求；SF 与带宽须配对：BW125→SF≤9） |

## 驱动兼容

RadioLib 已统一支持 SX126x（含 LLCC68）、SX127x、SX128x、RF69 等，加新射频优先复用 RadioLib。
国产芯片不在 RadioLib 覆盖内时，自写驱动，但对外接口不变。

优先复用库、兼容不了的才自写，是这个模块长期可扩展的前提。
