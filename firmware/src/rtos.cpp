// ============================================================
// rtos.cpp —— 通信基础设施实现：队列/事件组的统一定义与创建
// 队列/事件组【只在这里创建】，幂等，必须最先调用。
// ============================================================
#include "rtos.h"

// 队列/事件组句柄定义
QueueHandle_t q_rf_rx     = nullptr;
QueueHandle_t q_mac_tx_hi  = nullptr;
QueueHandle_t q_mac_tx_med = nullptr;
QueueHandle_t q_mac_tx_lo  = nullptr;
QueueHandle_t q_uart_rx   = nullptr;
QueueHandle_t q_usb_rx    = nullptr;
QueueHandle_t q_uart_tx   = nullptr;
QueueHandle_t q_usb_tx    = nullptr;
EventGroupHandle_t evg    = nullptr;

static bool rtos_inited = false;

static uint32_t g_tx_drop = 0;

void rtos_tx_drop_note(void) { g_tx_drop++; }
uint32_t rtos_tx_drop_count(void) { return g_tx_drop; }

void rtos_comms_init() {
    if (rtos_inited) return;
    rtos_inited = true;

    q_rf_rx      = xQueueCreate(Q_RF_RX_LEN,     sizeof(queue_item_t));
    q_mac_tx_hi  = xQueueCreate(Q_MAC_TX_HI_LEN, sizeof(queue_item_t));
    q_mac_tx_med = xQueueCreate(Q_MAC_TX_MED_LEN, sizeof(queue_item_t));
    q_mac_tx_lo  = xQueueCreate(Q_MAC_TX_LO_LEN, sizeof(queue_item_t));
    q_uart_rx    = xQueueCreate(Q_UART_RX_LEN,   sizeof(queue_item_t));
    q_usb_rx     = xQueueCreate(Q_USB_RX_LEN,    sizeof(queue_item_t));
    q_uart_tx    = xQueueCreate(Q_UART_TX_LEN,   sizeof(queue_item_t));
    q_usb_tx     = xQueueCreate(Q_USB_TX_LEN,    sizeof(queue_item_t));

    evg = xEventGroupCreate();
}
