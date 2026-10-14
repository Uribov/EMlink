// ============================================================
// usb_cdc.cpp —— 双虚拟串口：CDC0=MAVLink(Serial)，CDC1=CLI(Serial1)
// 需 platformio.ini build_flags：
//   -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1（双 CDC 描述符）
// ============================================================
#include "usb_cdc.h"
#include "../rtos.h"

static TaskHandle_t g_usb_task = nullptr;

void usb_cdc_init() {
    // Serial/Serial1 由 Arduino 框架按 CDC-on-boot 自动初始化，无需手动 begin
}

// ---- CDC0：MAVLink 数据通路（QGC）----
bool   usb_mav_available()            { return Serial.available() > 0; }
size_t usb_mav_read(uint8_t* b, size_t n) { return Serial.read(b, n); }
size_t usb_mav_write(const uint8_t* b, size_t n) { return Serial.write(b, n); }

// ---- CDC1：CLI 命令行（配置/调试）----
bool   usb_cli_available()            { return Serial1.available() > 0; }
size_t usb_cli_read(uint8_t* b, size_t n) { return Serial1.read(b, n); }
size_t usb_cli_write(const uint8_t* b, size_t n) { return Serial1.write(b, n); }

// ---- usb_task：USB CDC0(MAVLink) <-> 队列 ----
static void usb_task_func(void* arg) {
    (void)arg;
    uint8_t rx[FRAME_MTU];
    for (;;) {
        // 收：CDC0 -> q_usb_rx（mav_router 消费）
        size_t avail = Serial.available();
        if (avail) {
            size_t n = Serial.read(rx, (avail < sizeof(rx)) ? avail : sizeof(rx));
            queue_item_t item = {};
            memcpy(item.data, rx, n);
            item.len = (uint16_t)n;
            xQueueSend(q_usb_rx, &item, 0);
        }
        // 发：q_usb_tx -> CDC0（mav_router 生产）
        queue_item_t tx;
        while (xQueueReceive(q_usb_tx, &tx, 0) == pdTRUE) {
            Serial.write(tx.data, tx.len);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void usb_cdc_start(void) {
    xTaskCreatePinnedToCore(usb_task_func, "usb", USB_TASK_STACK,
                            nullptr, USB_TASK_PRIO, &g_usb_task, USB_TASK_CORE);
}
