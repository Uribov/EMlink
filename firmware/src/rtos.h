#pragma once
// ============================================================
// rtos.h —— 通信基础设施 + 任务调度参数（唯一真相源）
//
// 职责分工（仿 PX4：模块自包含 + 启动脚本化）：
//   - 本文件：队列/事件组句柄 + 任务参数宏（核/优先级/栈）+ 事件位 + 队列元素类型
//   - 各模块 .cpp：static 任务函数 + static 任务句柄 + xxx_start()
//   - main.cpp setup()：先 rtos_comms_init()，再按角色依次 xxx_start()
//
// 约定：
//   - 队列/事件组【只由 rtos_comms_init() 统一创建】，模块 start 内禁止再建
//   - rtos_comms_init() 幂等（static bool inited），必须最先调用
// ============================================================
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include "link/frame_codec.h"   // FRAME_MTU（队列元素字节块上限）

// ============================================================
// 任务调度参数（架构 §3.1）：核 / 优先级 / 栈
// 供各模块 xxx_start() 里 xTaskCreatePinnedToCore 引用
// 注：本项目 LoRa 不开 WiFi/BT，core0 空闲给射频；将来加 WiFi(OTA) 需重估核心
// ============================================================
#define RF_TASK_CORE        0
#define RF_TASK_PRIO        22
#define RF_TASK_STACK       4096

#define MAC_TASK_CORE       0
#define MAC_TASK_PRIO       20
#define MAC_TASK_STACK      4096

#define MESH_TASK_CORE      0
#define MESH_TASK_PRIO      18
#define MESH_TASK_STACK     4096

#define MAVLINK_TASK_CORE   1
#define MAVLINK_TASK_PRIO   12
#define MAVLINK_TASK_STACK  8192

#define UART_TASK_CORE      1
#define UART_TASK_PRIO      10
#define UART_TASK_STACK     4096

#define USB_TASK_CORE       1
#define USB_TASK_PRIO       10
#define USB_TASK_STACK      4096

#define CLI_TASK_CORE       1
#define CLI_TASK_PRIO       8
#define CLI_TASK_STACK      4096

#define STATS_TASK_CORE     1
#define STATS_TASK_PRIO     4
#define STATS_TASK_STACK    2048

// ============================================================
// 队列（架构 §3.2）：长度 + 所有权（生产方 → 消费方）
// 句柄统一由 rtos_comms_init() 创建，各模块只引用不创建
// ============================================================
#define Q_RF_RX_LEN         16    // radio → mesh，原始帧

// 发送优先级队列（QOS2 > QOS1 > QOS0，架构 §8.1 排队优先级）
#define Q_MAC_TX_HI_LEN     8     // QOS2：控制指令（心跳/命令）
#define Q_MAC_TX_MED_LEN    8     // QOS1：参数/航点
#define Q_MAC_TX_LO_LEN     16    // QOS0：遥测

#define Q_UART_RX_LEN       8     // uart_fc → mav_router，飞控字节流块
#define Q_USB_RX_LEN        8     // usb_cdc → mav_router，地面站字节流块
#define Q_UART_TX_LEN       8     // mav_router → uart_fc，下发字节流块
#define Q_USB_TX_LEN        8     // mav_router → usb_cdc，上送字节流块
#define Q_FRAG_LEN          8     // mav_router → frag，待重组分片

// ---- 队列元素类型（统一字节块 + 元数据，各队列复用）----
//   q_rf_rx          用 data/len/rssi/snr（原始线上帧 + 信号）
//   q_mac_tx_*       用 data/len/qos/ack_req（待发帧 + QoS）
//   q_uart_rx 等流队列 只用 data/len（字节流块）
//   q_frag           用 data/len（分片帧，frag_hdr 已在帧内偏移 10~13）
typedef struct {
    uint8_t  data[FRAME_MTU];
    uint16_t len;
    uint8_t  qos;      // QOS0/1/2（q_mac_tx_* 用）
    uint8_t  ack_req;  // 0/1（q_mac_tx_* 用）
    int16_t  rssi;     // 信号（q_rf_rx 用）
    int16_t  snr;
} queue_item_t;

// 队列句柄（extern 声明，rtos_comms_init 里定义并创建）
extern QueueHandle_t q_rf_rx;
extern QueueHandle_t q_mac_tx_hi;
extern QueueHandle_t q_mac_tx_med;
extern QueueHandle_t q_mac_tx_lo;
extern QueueHandle_t q_uart_rx;
extern QueueHandle_t q_usb_rx;
extern QueueHandle_t q_uart_tx;
extern QueueHandle_t q_usb_tx;
extern QueueHandle_t q_frag;

// ---- 按 QoS 投递发送队列（供 mesh/mav_router 统一调用）----
static inline bool rtos_tx_enqueue(const queue_item_t* item) {
    if (!item) return false;
    if (item->qos >= QOS2) return xQueueSend(q_mac_tx_hi, item, 0) == pdTRUE;
    if (item->qos == QOS1) return xQueueSend(q_mac_tx_med, item, 0) == pdTRUE;
    return xQueueSend(q_mac_tx_lo, item, 0) == pdTRUE;
}

// ============================================================
// 事件组 + 事件位（架构 §3.3）
// ============================================================
#define EV_USB_PLUG         (1 << 0)   // power_detect：USB 插入
#define EV_USB_UNPLUG       (1 << 1)   // power_detect：USB 拔除
#define EV_LINK_LOST        (1 << 2)   // stats_task：3s 无上行
#define EV_LINK_RESTORED    (1 << 3)   // mac_task：ACK 恢复
#define EV_RF_FAULT         (1 << 4)   // radio：连续无响应
#define EV_CFG_CHANGED      (1 << 5)   // cli_task：save 后

// 事件组句柄（extern 声明，rtos_comms_init 里定义并创建）
extern EventGroupHandle_t evg;

// ============================================================
// rtos_comms_init：只创建队列/事件组（幂等，必须最先调用）
// 任务创建分散在各模块 xxx_start()，不由本函数负责
// ============================================================
void rtos_comms_init();
