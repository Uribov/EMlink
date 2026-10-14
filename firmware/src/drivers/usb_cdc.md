# usb_cdc —— 双 USB 虚拟串口

## 职责
ESP32-S3 原生 USB-OTG 拆成两个 CDC：**CDC0 走 MAVLink（QGC）**，**CDC1 走 CLI**。
地面站数据流和命令行互不干扰。

## 依赖
- ESP32-S3 USB 外设 + TinyUSB（Arduino 框架内置）
- 架构 §8.2 电源状态机：飞行中插 USB 只开 CDC1(CLI)，**不切数据通路**

## 关键实现点
1. 双 CDC 需在 `platformio.ini` 的 `build_flags` 加：
   `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`，并配置 TinyUSB 描述符为 2 路 CDC
   （`Serial`=CDC0，`Serial1`=CDC1）
2. 本模块封装 `Serial`/`Serial1`，上层（`usb_task`）只认 `usb_mav_*`/`usb_cli_*`

## 对外接口
见 `usb_cdc.h`。要点：`usb_cdc_init / usb_cdc_start / usb_mav_write / usb_cli_write` + `usb_cdc_drop_count()`（下行队列满丢弃计数，P1-10）。

## 验收（P0）
- 插 USB 设备管理器出现两个 COM；CDC0 透传 MAVLink，CDC1 能敲命令回显

## 当前实现架构（供人和子 agent 阅读）

### 一句话
S3 的原生 USB 拆成**两个虚拟串口**：CDC0 走 MAVLink（给地面站 QGC），CDC1 走命令行（配置/调试）。两路互不干扰。

### 串口映射
| 逻辑口 | Arduino 对象 | 用途 |
|---|---|---|
| CDC0 | `Serial` | MAVLink 数据（QGC ↔ mav_router） |
| CDC1 | `Serial1` | CLI 命令行（cli 模块） |

### 数据流（usb_task，核1 优先级10，1ms 轮询）
```
QGC ──> Serial（CDC0）──> q_usb_rx ──> mav_router
QGC <── Serial <── q_usb_tx <── mav_router
```

### 双 CDC 的开启条件
`platformio.ini` 的 build_flags 里必须有两行（已配）：
`-DARDUINO_USB_MODE=1`（USB-OTG）+ `-DARDUINO_USB_CDC_ON_BOOT=1`（CDC on boot）
缺了就只有单 CDC，`Serial1` 会编译不过。

### 安全约定（架构 §8.2）
飞行中（AIR 态）插 USB 只开 CDC1(CLI)，**不切数据通路**——这个切换逻辑在 failsafe_policy，不在本模块。
