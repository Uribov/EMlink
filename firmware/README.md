# 固件工程说明 + 大模型接入指南

> 总架构见上一级目录 `mesh_firmware_architecture.md`。本目录是 PlatformIO 工程骨架。
> **第一次做嵌入式？先读 `docs/CONCEPTS.md`（Arduino / PlatformIO / FreeRTOS 大白话科普）。**

## 1. 目录结构

```
firmware/
  platformio.ini          # 多方案配置（每个 [env] = 一块板 + 一个射频）
  scripts/build_all.sh    # 批量编译所有方案（Linux）
  lib/                    # 本地依赖库：mavlink2 + radiolib + neopixel（全离线）
  docs/
    CONCEPTS.md           # 基础概念（Arduino/PlatformIO/FreeRTOS 大白话科普）
    MODULE_INDEX.md       # 模块索引 + 依赖图 + 实现顺序
    HARDWARE_VARIANTS.md  # 多方案架构（怎么加新板/新射频）
    tool.md               # 环境工具清单（Windows/Linux 搭建）
  src/
    main.cpp              # 入口（按角色装配启动所有模块）
    pinmap.h              # 引脚入口（按宏 include boards/ + radios/）
    rtos.h                # FreeRTOS 对象清单（任务/队列/事件，唯一真相源）
    boards/               # 板级引脚（每块 MCU 板一个 .h）
    radios/               # 射频引脚（每个射频模块一个 .h）
    drivers/              # 驱动层：radio / uart_fc / usb_cdc / power_detect
    link/                 # 链路层：frame_codec / mac / duty / adr(预留)
    net/                  # 网络层：mesh / route
    service/              # 服务层：mav_router / frag / addr_map / nvs_cfg / watchdog / crypto(预留) / ota(预留)
    app/                  # 应用层：role / cli / stats / failsafe_policy
```

## 2. 每个模块 = 一个 `.h` + 一个 `.md`（双文件约定）

| 文件 | 是什么 | 给谁看 |
|---|---|---|
| `<模块>.h` | **接口契约**：函数签名、结构体、常量、`#define` | 大模型照着它生成 `<模块>.cpp` 实现 |
| `<模块>.md` | **规格说明**：职责、依赖、核心算法、验收标准 | 大模型理解"为什么/怎么做" |

**`.md` 是给大模型的上下文，`.h` 是它要兑现的契约。**

## 3. 怎么接入大模型开发代码（推荐用法）

一次只喂一个模块，提示词模板：

```
读 EMlink/firmware/src/link/frame_codec.md 和 frame_codec.h，
以及 ../mesh_firmware_architecture.md 第 4 节的帧格式。
按 .h 里声明的接口实现 frame_codec.cpp，帧格式严格按架构文档，
只实现这个模块，不要改动其他文件，不要引入未声明的依赖。
```

要点：
1. **单模块单次**——上下文聚焦，质量最高。
2. **必给架构文档**——帧格式、地址、TTL、QoS 等全局约定在总架构里，别漏。
3. **让大模型"只写 .cpp"**——接口（.h）已定死，避免它乱改契约。
4. **实现顺序按 §11**：先 frame_codec → mac → mesh → frag → mav_router → ……

## 4. 编译 / 烧录

- VSCode + PlatformIO 扩展：左下角 **✓ Build** 编译，**→ Upload** 烧录，**🔌 Serial Monitor** 看日志。
- 或命令行（需 PlatformIO Core CLI 在 PATH）：
  ```
  pio run -t upload
  pio device monitor
  ```
- 依赖库已全部本地化到 `lib/`（mavlink2 + radiolib + neopixel），**无需联网下载**。

## 5. 当前状态

| 阶段 | 状态 |
|---|---|
| P0~P4 代码（18 模块） | 全实现 + 编译通过，待硬件 bring-up |
| 文档 | 18 模块 .md 均「约束 + 实现架构」双结构 |

## 6. 上板前唯一待确认（非阻塞）

`platformio.ini` 里 `board = esp32-s3-devkitc-1` 是通用 ESP32-S3 配置。
若烧录报错 / 串口不对 / Flash 大小不匹配，按你板子实际改 board 与 `board_build.flash_size`。
引脚映射已在 `pinmap.h` 填全（LoRa SPI + 飞控 UART0 + WS2818 LED）。
