# power_detect —— USB 连上检测

## 职责
判断「USB 有没有连上电脑」，输出事件给电源/接口状态机（防双供电，架构 §8.2）。

## 为什么不用测电压
C 口和 4Pin 供电时，S3 的 5V 引脚**都有 5V**，量电压分不清是谁在供电。
所以判断 USB 数据通路只能靠 **TinyUSB 枚举状态**（是否与主机握手成功）。

## 依赖
- TinyUSB（Arduino 内置）
- FreeRTOS 事件组：EV_USB_PLUG / EV_USB_UNPLUG

## 关键实现点
1. 用 TinyUSB 的 `tud_mounted()`（设备已枚举）或等效回调判断 USB 是否连上主机
2. 状态变化时去抖 ~50ms 后 set/clear event bit（EV_USB_PLUG / EV_USB_UNPLUG）
3. 无额外 GPIO：不占引脚，供电电路无需 VBUS 分压检测脚

## 对外接口
见 `power_detect.h`。

## 验收（P0）
- 插 USB 到电脑 → EV_USB_PLUG 一次；拔掉 → EV_USB_UNPLUG 一次，无重复触发
- 只插 4Pin（无 USB 主机）→ 不触发 EV_USB_PLUG
