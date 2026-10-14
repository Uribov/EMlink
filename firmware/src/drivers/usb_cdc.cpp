// ============================================================
// usb_cdc.cpp —— 单口分流：所有 USB 数据走 Serial 一个口
//   usb_task 按【字节】区分：
//     0xFD/0xFE 开头 -> 按帧收完整 MAVLink 帧 -> q_usb_rx -> mav_router
//     其余字节        -> CLI 文本环形缓冲      -> usb_cli_read
// 依赖：ESP32-S3 USB-OTG（Arduino 内置）；规格：usb_cdc.md
// 注：本工程 platformio.ini 未配置 TinyUSB 复合描述符（无第二个 CDC），
//     故不再使用 Serial1；CLI 与 MAVLink 共用一个虚拟串口。
// ============================================================
#include "usb_cdc.h"
#include "../rtos.h"
#include <freertos/semphr.h>

static TaskHandle_t g_usb_task = nullptr;
static uint32_t g_rx_drop = 0;    // q_usb_rx 队列满丢弃（P1-10）
static uint32_t g_cli_drop = 0;   // CLI 环形缓冲满丢弃

// ---- §1 发送方向：一个互斥锁保护 Serial 的写 ----
// Serial 会被两个来源写：usb_task（MAVLink 出）和 cli_task（CLI 回显）。
// 不加保护会把 CLI 文本插进 MAVLink 帧中间，QGC 解析出错。
// 用互斥量而非再队列：CLI 需要立刻回显（队列做不到同步），
// 且 MAVLink 侧本就由 usb_task 独占写，加锁只为防 CLI 插队。
static SemaphoreHandle_t g_usb_tx_lock = nullptr;

// ---- §2.3 CLI 文本环形缓冲（单写者 = usb_task，单读者 = cli_task）----
#define CLI_RING_SZ 256
static uint8_t           g_cli_ring[CLI_RING_SZ];
static volatile uint16_t g_cli_head = 0;   // 写指针（usb_task）
static volatile uint16_t g_cli_tail = 0;   // 读指针（cli_task）

static void cli_ring_push(uint8_t b) {
    uint16_t next = (uint16_t)((g_cli_head + 1) % CLI_RING_SZ);
    if (next == g_cli_tail) { g_cli_drop++; return; }   // 满：丢最新字节并计数
    g_cli_ring[g_cli_head] = b;
    g_cli_head = next;
}

void usb_cdc_init() {
    // Serial 由 Arduino 框架按 CDC-on-boot 自动初始化，无需手动 begin
    if (!g_usb_tx_lock) g_usb_tx_lock = xSemaphoreCreateMutex();
}

// ---- MAVLink 数据通路（QGC，走共用的 Serial 口）----
// 注意：接收已由 usb_task 统一接管（见 §2 分流），usb_mav_read/available
// 仅为兼容保留，不再作为接收路径使用。
bool   usb_mav_available()            { return false; }
size_t usb_mav_read(uint8_t* b, size_t n) { (void)b; (void)n; return 0; }
size_t usb_mav_write(const uint8_t* b, size_t n) {
    if (g_usb_tx_lock) xSemaphoreTake(g_usb_tx_lock, portMAX_DELAY);
    size_t w = Serial.write(b, n);
    if (g_usb_tx_lock) xSemaphoreGive(g_usb_tx_lock);
    return w;
}

// ---- CLI 命令行（配置/调试，走共用的 Serial 口）----
bool usb_cli_available() { return g_cli_head != g_cli_tail; }

// 语义不变：逐字节读；由 cli_task 从环形缓冲搬走
size_t usb_cli_read(uint8_t* b, size_t n) {
    size_t got = 0;
    while (got < n && g_cli_head != g_cli_tail) {
        b[got++] = g_cli_ring[g_cli_tail];
        g_cli_tail = (uint16_t)((g_cli_tail + 1) % CLI_RING_SZ);
    }
    return got;
}

size_t usb_cli_write(const uint8_t* b, size_t n) {
    if (g_usb_tx_lock) xSemaphoreTake(g_usb_tx_lock, portMAX_DELAY);
    size_t w = Serial.write(b, n);
    if (g_usb_tx_lock) xSemaphoreGive(g_usb_tx_lock);
    return w;
}

// ============================================================
// §2 接收方向：usb_demux_byte —— 逐字节状态机（Serial 的唯一读者 = usb_task）
//   空闲 / 待帧长 / 收帧中；帧长公式见 §2.1。
// ============================================================
#define MAV_FRAME_BUF_MAX       280   // MAVLink v2 上限 279
#define DEMUX_FRAME_TIMEOUT_MS  500   // §2.2 卡帧保护

enum demux_state_t { DEMUX_IDLE = 0, DEMUX_HDR_LEN, DEMUX_RECV };

static uint8_t  g_demux_state   = DEMUX_IDLE;
static uint8_t  g_frame_buf[MAV_FRAME_BUF_MAX];
static uint16_t g_frame_len     = 0;   // 目标帧总长
static uint16_t g_frame_pos     = 0;   // 已收字节数
static uint32_t g_frame_last_ms = 0;   // 最近收到帧内字节的时刻

// 投递完整帧。q_usb_rx 元素仅 FRAME_MTU(200) 字节，而 MAVLink 帧可达
// 279 字节；mav_router.feed_usb 是字节流消费者，故按 FRAME_MTU 分片投递。
static void usb_frame_deliver(void) {
    uint16_t off = 0;
    while (off < g_frame_len) {
        uint16_t chunk = (uint16_t)(g_frame_len - off);
        if (chunk > FRAME_MTU) chunk = FRAME_MTU;
        queue_item_t item = {};
        memcpy(item.data, g_frame_buf + off, chunk);
        item.len = chunk;
        if (xQueueSend(q_usb_rx, &item, 0) != pdTRUE) g_rx_drop++;   // 队列满丢弃（P1-10）
        off += chunk;
    }
    g_demux_state = DEMUX_IDLE;
    g_frame_pos = 0;
    g_frame_len = 0;
}

static void usb_demux_byte(uint8_t b) {
    // §2.2 卡帧保护：收帧中超过 500ms 无新字节 -> 复位，丢弃半截帧，
    // 再把当前字节当作新起点（否则一次误判会永久卡死接收方向）。
    if (g_demux_state != DEMUX_IDLE &&
        (uint32_t)(millis() - g_frame_last_ms) > DEMUX_FRAME_TIMEOUT_MS) {
        g_demux_state = DEMUX_IDLE;
        g_frame_pos = 0;
        g_frame_len = 0;
    }

    switch (g_demux_state) {
    case DEMUX_IDLE:
        if (b == 0xFD || b == 0xFE) {          // MAVLink v2 / v1 帧头
            g_frame_buf[0] = b;
            g_frame_pos = 1;
            g_frame_len = 0;
            g_frame_last_ms = millis();
            g_demux_state = DEMUX_HDR_LEN;
        } else {
            cli_ring_push(b);                  // 其余 -> CLI 文本
        }
        break;

    case DEMUX_HDR_LEN: {                      // 第 2 字节 = payload 长度
        g_frame_buf[g_frame_pos++] = b;
        // v2: 0xFD|len|incompat|compat|seq|sysid|compid|msgid[3]|payload|crc[2]
        //     -> 总长 = 10 + len + 2
        // v1: 0xFE|len|seq|sysid|compid|msgid|payload|crc[2]
        //     -> 总长 = 6 + len + 2
        // （暂不含 MAVLink 签名：本工程不用；多余签名字节下一轮按文本处理）
        uint16_t total = (uint16_t)((g_frame_buf[0] == 0xFD ? 10u : 6u) + b + 2u);
        g_frame_last_ms = millis();
        if (total > MAV_FRAME_BUF_MAX) {       // 异常长度 -> 丢弃并复位
            g_demux_state = DEMUX_IDLE;
            g_frame_pos = 0;
            g_frame_len = 0;
        } else {
            g_frame_len = total;
            g_demux_state = DEMUX_RECV;
        }
        break;
    }

    case DEMUX_RECV:
        if (g_frame_pos < MAV_FRAME_BUF_MAX) g_frame_buf[g_frame_pos] = b;
        g_frame_pos++;
        g_frame_last_ms = millis();
        if (g_frame_pos >= g_frame_len) usb_frame_deliver();
        break;
    }
}

// ---- usb_task：Serial <-> q_usb_rx / q_usb_tx + CLI 文本分流 ----
static void usb_task_func(void* arg) {
    (void)arg;
    uint8_t rx[FRAME_MTU];
    for (;;) {
        // 收：Serial -> 逐字节分流
        size_t avail = Serial.available();
        if (avail) {
            size_t n = Serial.read(rx, (avail < sizeof(rx)) ? avail : sizeof(rx));
            for (size_t i = 0; i < n; i++) usb_demux_byte(rx[i]);
        }
        // 发：q_usb_tx -> Serial（mav_router 生产）
        queue_item_t tx;
        while (xQueueReceive(q_usb_tx, &tx, 0) == pdTRUE) {
            usb_mav_write(tx.data, tx.len);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void usb_cdc_start(void) {
    xTaskCreatePinnedToCore(usb_task_func, "usb", USB_TASK_STACK,
                            nullptr, USB_TASK_PRIO, &g_usb_task, USB_TASK_CORE);
}

uint32_t usb_cdc_drop_count(void) { return g_rx_drop; }
uint32_t usb_cli_drop_count(void) { return g_cli_drop; }
