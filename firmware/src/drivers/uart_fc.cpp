// ============================================================
// uart_fc.cpp —— 与飞控(Pixhawk)的 UART（UART0，GPIO43/44）
// 仅 AIR 角色使用。uart_task 搬运 UART <-> 队列。
// ============================================================
#include "uart_fc.h"
#include "../rtos.h"

static TaskHandle_t g_uart_task = nullptr;

void uart_fc_init(uint32_t baud) {
    Serial0.begin(baud, SERIAL_8N1, PIN_FC_RX, PIN_FC_TX);
}

size_t uart_fc_available() {
    return Serial0.available();
}

size_t uart_fc_read(uint8_t* buf, size_t maxlen) {
    if (!buf || maxlen == 0) return 0;
    return Serial0.read(buf, maxlen);
}

size_t uart_fc_write(const uint8_t* buf, size_t len) {
    if (!buf || len == 0) return 0;
    return Serial0.write(buf, len);
}

// ---- uart_task：飞控 UART <-> 队列 ----
static void uart_task_func(void* arg) {
    (void)arg;
    uint8_t rx[FRAME_MTU];
    for (;;) {
        // 收：飞控 -> q_uart_rx（mav_router 消费）
        size_t avail = Serial0.available();
        if (avail) {
            size_t n = Serial0.read(rx, (avail < sizeof(rx)) ? avail : sizeof(rx));
            queue_item_t item = {};
            memcpy(item.data, rx, n);
            item.len = (uint16_t)n;
            xQueueSend(q_uart_rx, &item, 0);
        }
        // 发：q_uart_tx -> 飞控（mav_router 生产）
        queue_item_t tx;
        while (xQueueReceive(q_uart_tx, &tx, 0) == pdTRUE) {
            Serial0.write(tx.data, tx.len);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void uart_fc_start(void) {
    xTaskCreatePinnedToCore(uart_task_func, "uart", UART_TASK_STACK,
                            nullptr, UART_TASK_PRIO, &g_uart_task, UART_TASK_CORE);
}
