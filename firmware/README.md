# 固件工程说明

> 总架构见上一级目录 `mesh_firmware_architecture.md`。本目录是 PlatformIO 工程骨架。
> 第一次做嵌入式，先读 `docs/CONCEPTS.md`（Arduino / PlatformIO / FreeRTOS 基础概念）。
> 准备上板，先读 `../docs/BRINGUP.md`（接线、整片擦除、开机看什么、现象对照表）。

## 1. 目录结构

```
firmware/
  platformio.ini          # 多方案配置（每个 [env] = 一块板 + 一个射频）
  partitions_ota.csv      # OTA 双分区表（app0/app1 各 1.5M + otadata 回滚）
  scripts/build_all.sh    # 批量编译所有方案（Linux）
  lib/                    # 本地依赖库：mavlink2 + radiolib + neopixel
  docs/
    CONCEPTS.md           # 基础概念（Arduino/PlatformIO/FreeRTOS）
    MODULE_INDEX.md       # 模块索引 + 依赖图 + 实现顺序
    HARDWARE_VARIANTS.md  # 多方案架构（怎么加新板/新射频）
    tool.md               # 环境工具清单（装环境 / 编译 / 串口权限 / 推送）
  src/
    main.cpp              # 入口（按角色装配启动所有模块）
    pinmap.h              # 引脚入口（按宏 include boards/ + radios/）
    rtos.h                # FreeRTOS 对象清单（任务/队列/事件，唯一真相源）
    boards/               # 板级引脚（每块 MCU 板一个 .h）
    radios/               # 射频引脚（每个射频模块一个 .h）
    drivers/              # 驱动层：radio / airtime / uart_fc / usb_cdc / power_detect
    link/                 # 链路层：frame_codec / mac / duty / adr
    net/                  # 网络层：mesh / route
    service/              # 服务层：mav_router / frag / addr_map / nvs_cfg / watchdog / crypto / ota / version / role_state
    app/                  # 应用层：role / cli / stats / failsafe_policy
```

## 2. 每个模块 = 一个 `.h` + 一个 `.md`（双文件约定）

| 文件 | 是什么 | 给谁看 |
|---|---|---|
| `<模块>.h` | 接口契约：函数签名、结构体、常量、`#define` | 照着它生成 `<模块>.cpp` 实现 |
| `<模块>.md` | 规格说明：前半「编程约束」、后半「实现后的详细架构」 | 人和 AI 理解为什么这么写 |

`.md` 是上下文，`.h` 是要兑现的契约。改代码必须同步对应的 `.md`。

## 3. 怎么开发代码

> 当前实际流程（2026-10-15 起）：把任务书写到 `../docs/reviews/briefs/`，交给 AI（`bash ~/.local/bin/oc-deepseek '<调度提示词>' -f <任务书>`），人工逐行审 diff，编译，同步 `.md`，单项提交。
> 任务书里必须写明：允许改哪些文件（白名单）、逐条改法、自检问题、禁止事项（不许改 `.md`、不许编译、不许提交）。任务书是一次性工作文件，该项提交后即删除（长期记录看 `../docs/reviews/FIX_LOG.md`）。

要点（早期「单模块喂提示词」的做法，仍然适用）：

1. 单模块单次，上下文聚焦，质量最高。
2. 必须给架构文档。帧格式、地址、TTL、QoS 等全局约定在总架构里，别漏。
3. 让 AI 只写 `.cpp`。接口（`.h`）已定死，避免它乱改契约。
4. 平台硬约束必须先查文档：射频芯片是 LLCC68（驱动用 `SX1262` 类，BW125 时 SF 最高 9），依据见 `../../docs/references/LLCC68_E220-900M22S.md`。

## 4. 编译 / 烧录

- VSCode + PlatformIO 扩展：左下角的 Build 按钮编译，Upload 烧录，Serial Monitor 看日志。
- 或命令行（需 PlatformIO Core CLI 在 PATH）：
  ```
  pio run -t upload
  pio device monitor
  ```
- 依赖库已全部本地化到 `lib/`（mavlink2 + radiolib + neopixel），不需要联网下载。
- 环境搭建/串口权限/推送，见 `docs/tool.md`。

## 5. 当前状态

| 阶段 | 状态 |
|---|---|
| 模块 | 21 个模块，另加 `airtime`、`ratelimit`、`version`、`role_state` 等横切支撑件，全部实现，编译通过（RAM 17.4% / Flash 55.9%），待硬件 bring-up |
| 文档 | 每个模块 `.md` 均为「编程约束 + 实现架构」双结构；索引见 `docs/MODULE_INDEX.md` |
| 上板 | 开工先读 `../docs/BRINGUP.md`；批量烧录用 `../tools/flash_batch.sh` |
| 留档 | 审查/修复记录 `../docs/reviews/FIX_LOG.md`；平台硬约束 `../docs/references/LLCC68_E220-900M22S.md` |

## 6. 上板前注意事项

1. 首次烧录必须整片擦除。分区表已从 `default.csv`（单 app + spiffs）改成 `partitions_ota.csv`（双 app OTA），旧分区数据与新布局对不上，直接烧会起不来。首次用整片擦除：`pio run -t erase` 后再 `pio run -t upload`（或烧录时勾 "Erase Flash"）；也可以直接用 `../tools/flash_batch.sh`，它已内含整片擦除、合并镜像、逐板写入。之后正常 OTA 升级不用再擦。
2. `platformio.ini` 里 `board = esp32s3-devkitc-1` 是通用 ESP32-S3 配置。若烧录报错、串口不对或 Flash 大小不匹配，按你板子实际改 board 与 `board_build.flash_size`。
3. 引脚映射已在 `pinmap.h` 填全（LoRa SPI + 飞控 UART0 + WS2812 LED）。
4. 先装天线再上电，尤其设了高功率之后；别在室内让两台板贴着满功率长时间发射。
