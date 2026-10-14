#pragma once
// ============================================================
// uart_fc —— 与飞控(Pixhawk)的 UART，DMA + 环形缓冲
// 依赖：pinmap.h；规格：uart_fc.md
// ============================================================
#include <Arduino.h>
#include "../pinmap.h"

// 初始化；DMA 接收 + 环形缓冲
void uart_fc_init(uint32_t baud);

// 环形缓冲里可读字节数
size_t uart_fc_available();

// 读当前缓冲内所有字节（无阻塞）
size_t uart_fc_read(uint8_t* buf, size_t maxlen);

// 写（DMA/中断发送，非阻塞）
size_t uart_fc_write(const uint8_t* buf, size_t len);

// 启动飞控 UART 任务（内部 xTaskCreate uart_task_func，任务函数为模块内 static）
void uart_fc_start(void);
