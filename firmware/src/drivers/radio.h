#pragma once
// ============================================================
// radio —— 射频驱动抽象接口（唯一射频入口）
// 上层（mac/mesh）只认本接口，不直接碰 RadioLib 或具体芯片。
// 具体芯片在 radio.cpp 里按芯片类型宏分叉实现：
//   RADIO_SX126X（SX1261/2/8 + LLCC68）
//   RADIO_SX127X（SX1276/7/8，将来）
// 芯片类型宏由 src/radios/<射频>.h 定义。
// 依赖：RadioLib、pinmap.h；规格：radio.md
// ============================================================
#include <Arduino.h>
#include <RadioLib.h>
#include "../pinmap.h"

// ---- 默认射频参数（无 NVS 覆盖时）----
#define LORA_DEF_FREQ      915.0f
#define LORA_DEF_BW        125.0f
#define LORA_DEF_SF        7
#define LORA_DEF_CR        5        // 4/5
#define LORA_DEF_SYNCWORD  0x12     // 私有同步字
#define LORA_DEF_POWER     20       // dBm

// ---- 通用射频接口（所有芯片实现同一组，函数名统一 radio_*）----
// 初始化；返回 true 成功
bool radio_init(float freq, float bw, uint8_t sf, uint8_t cr,
                uint16_t sync, int8_t pwr);

// 射频是否就绪（init 成功且未被撤下）。未就绪时所有射频操作直接失败返回，不碰 SPI
bool radio_is_ready(void);

// 阻塞发送；返回 0 成功，负值错误码
int16_t radio_transmit(const uint8_t* data, size_t len, uint32_t timeout_ms);

// 高优先发送（ACK/控制帧用；与 radio_transmit 同语义，但排在普通数据帧前面）
// 注意：P0-2 起射频归 rf_task 单所有者——以上所有射频接口内部改为"请求队列 + 阻塞等结果"，
// 调用者（mac/mesh/ota/watchdog 等）不需要改调用方式；但调用这些接口的任务
// 不得把"任务通知（task notification）"用于其它用途。
int16_t radio_transmit_prio(const uint8_t* data, size_t len);

// 进入接收态；DIO 触发中断（由 rf_task 消费）
bool radio_startReceive();

// 读已收包；len 输出实际长度，rssi/snr 输出信号
int16_t radio_readPacket(uint8_t* buf, size_t maxlen, size_t* len,
                         float* rssi, float* snr);

// LBT 侦听（CAD）：0=空闲 1=忙 负=错误码
int16_t radio_channelActivity();

// 诊断
float radio_lastRssi();
float radio_lastSnr();

// 运行时切扩频因子（ADR 用）：standby -> setSpreadingFactor -> startReceive
// 返回 true 成功；切参数时芯片必须不在收发状态（SX126x 硬要求）
bool radio_set_sf(uint8_t sf);

// 复位射频芯片
void radio_hardReset();

// 暂停射频（OTA 维护模式用：WiFi 与 LoRa 二选一）。暂停后所有射频操作 no-op
void radio_suspend();
// 恢复射频（回接收态）
void radio_resume();

// 启动射频任务（内部 xTaskCreate rf_task_func，任务函数为模块内 static）
void radio_start(void);
