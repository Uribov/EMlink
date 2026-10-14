# adr —— 自适应速率（预留，未实现）

## 状态
预留占位，未实现。当前固定 SF7，够 500m 视距用。

## 将来做什么
- 根据 RSSI/SNR 动态调 SF/BW：近距离降 SF 提速，远距离升 SF 保链路
- 功率自适应

## 现在预留的钩子
- `radio.h` 已有 `radio_lastRssi()` / `radio_lastSnr()` 接口
- 调度位置在 link 层（与 `mac` / `duty` 同层）

## 位置
见架构 §13.3。
