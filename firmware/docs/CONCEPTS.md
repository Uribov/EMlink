# 基础概念

> 用大白话 + 生活类比解释项目涉及的核心概念。第一次做嵌入式开发的话，先读一遍这篇再往下走。
> 关联：`MODULE_INDEX.md`（模块怎么组织）、`HARDWARE_VARIANTS.md`（硬件怎么多方案）。

## 1. 系统的分层结构（从下到上）

```
硬件芯片（ESP32-S3 + LLCC68 射频）
   ↑ 跑在
FreeRTOS（调度器：负责「同时干多件事」）
   ↑ 被包含在
Arduino 框架（抹平不同单片机的差异，提供现成库）
   ↑ 你用它写代码
你的业务代码（Mesh 数传逻辑）
   ↑ 由它编译烧录
PlatformIO（工具：编译、管库、烧录、多方案）
```

## 2. Arduino = 手机生态 / 预制菜

Arduino 不是一块板子，是一个生态平台。

**类比一：手机生态**

- 开发板 = 手机硬件（Uno、ESP32-S3…芯片千差万别）
- Arduino 框架 = Android（统一硬件差异，App 开发者不用管手机里什么芯片）
- 库（RadioLib 等）= App Store 的 App（下载即用，不用懂底层）

**类比二：预制菜**

- 传统裸机开发 = 从种菜养猪生火开始（写寄存器、配时钟，点亮 LED 要写几十行晦涩代码）
- Arduino = 料理包（食材切好调料配好，`digitalWrite(13, HIGH)` 一行点亮）
- 库 = 现成酱料包（RadioLib 一行 `radio.begin()` 搞定整个 LoRa 通信）

两个类比说的是同一件事：Arduino 把底层细节包起来，你只需要写业务。

## 3. PlatformIO = 厨房（Arduino 是菜谱）

PlatformIO 和 Arduino 不是二选一，是配合关系：

- Arduino 框架 = 菜谱（你写代码遵循的语言 + 库）
- PlatformIO = 专业厨房（编译、管库、烧录、多方案的工具）
- Arduino IDE = 官方附赠的简易小厨房（够做小菜，不够做你这种大菜）

`platformio.ini` 里 `framework = arduino` 就是在说：「用 PlatformIO 这个厨房，按 Arduino 的菜谱做。」

选 PlatformIO 的原因：你的项目需要多方案（多 `[env]`）、依赖自动管理（`lib_deps`）、批量编译（`build_all.sh`），这些 Arduino IDE 都做不利索。

## 4. FreeRTOS = 工人调度器

FreeRTOS 负责让芯片「看起来同时干多件事」：

| 机制 | 生活类比 |
|---|---|
| 任务（Task） | 一个独立的小工人 |
| 队列（Queue） | 工人之间的传送带 |
| 事件组（Event Group） | 信号旗 |
| 任务通知（Notification） | 中断专用的「敲门」 |

所有任务/队列/事件的句柄、优先级、事件位，统一定义在 `src/rtos.h`（唯一真相源）。

## 5. FreeRTOS 如何进入芯片

它不需要单独安装 —— FreeRTOS 是「随固件一起编译烧录进去的调度库」。

```
你的代码 + FreeRTOS 源码 + Arduino 框架  ──编译链接──→  firmware.bin ──烧录──→ flash
```

ESP32 的 Arduino 框架底层本来就是 FreeRTOS，板子从上电起 FreeRTOS 就在跑，你 `xTaskCreate` 只是「往已经在跑的系统里加任务」。

## 6. 小结

> 你用 PlatformIO（厨房），按 Arduino 框架（菜谱）写代码，代码里用 FreeRTOS（调度器）组织多任务，最后三者编译成一个 `.bin` 烧进 ESP32-S3（硬件），上电后 FreeRTOS 开始调度你的任务，Mesh 数传就跑起来了。
