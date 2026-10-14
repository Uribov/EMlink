# ota —— OTA 固件升级（WiFi，预留，未实现）

## 状态
预留占位，未实现。节点落地维护时连 WiFi 快速升级固件。

## 将来做什么
- 连 WiFi，用 `esp_ota` 下载固件并升级（LoRa 速率太低不适合传固件，参数同步走 §13.4）
- `partition` 表改用 OTA 双分区（ota_0 / ota_1 + otadata 回滚）
- 4MB Flash 双 OTA（1.5M×2）+ otadata + NVS 够用，不加大的 spiffs
- 流程：连 WiFi → 下载固件 → 校验 → 写 ota_1 → 重启切分区

## 现在预留的钩子
- `partition` 表当前是 `default.csv`（4MB），将来改 OTA 双分区
- 依赖 WiFi 栈（占 core0），届时重估核心分配

## 位置
见架构 §13.2。
