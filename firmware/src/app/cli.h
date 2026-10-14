#pragma once
// ============================================================
// cli —— USB CDC1 命令行
// 依赖：usb_cdc.h、nvs_cfg.h、stats.h；命令约定见架构 §9
// ============================================================
#include <Arduino.h>

// 初始化（注册命令、启动 cli_task）
void cli_init();

// 处理一行输入（cli_task 循环调用）
void cli_handle_line(const char* line);

// 打印横幅 / 帮助
void cli_banner();
void cli_help();

// 启动 CLI 任务（内部 xTaskCreate cli_task_func，任务函数为模块内 static）
void cli_start(void);
