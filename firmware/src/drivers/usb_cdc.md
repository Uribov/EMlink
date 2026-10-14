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
见 `usb_cdc.h`。

## 验收（P0）
- 插 USB 设备管理器出现两个 COM；CDC0 透传 MAVLink，CDC1 能敲命令回显
