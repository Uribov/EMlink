#pragma once
// ============================================================
// ota —— OTA 固件升级 / WiFi Web 管理（学 ELRS）
// 方案：WiFi AP 模式（SSID EMlink-<node_id>，IP 192.168.4.1）
//       Arduino WebServer + 内嵌 HTML（页面资源在 webui.h）：改参数 / 设密钥 / 刷固件(Update.h)
//       CLI `wifi` 命令触发；进入维护模式时 LoRa 暂停（二选一）
// 依赖：WiFi/WebServer/Update（ESP32 Arduino 内置）、nvs_cfg、crypto、radio、webui
// 规格：ota.md
// ============================================================
#include <Arduino.h>

// 启动 WiFi 维护模式：暂停 LoRa → 起 AP + WebServer + 内嵌 HTML
void ota_start();

// 退出维护模式：停 WebServer + 关 WiFi → 恢复 LoRa
void ota_stop();

// 是否处于维护模式
bool ota_active();

// 开机自检：系统启动且射频就绪后调用一次 → 向 bootloader 确认"本固件可用"（取消回滚）
// 由 main.cpp 的 loop() 每轮调用（内部自带"只做一次"保护）
void ota_boot_confirm_tick(void);
