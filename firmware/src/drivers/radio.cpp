// ============================================================
// radio.cpp —— 射频驱动抽象实现（唯一射频入口）
// 上层只认 radio_*，不直接碰 RadioLib。芯片按宏分叉：
//   RADIO_SX126X：SX1261/2/8 + LLCC68
// ============================================================
#include "radio.h"
#include "../rtos.h"
#include "../service/watchdog.h"

#if defined(RADIO_SX126X)
static SX1268* g_radio = nullptr;
static Module* g_mod   = nullptr;
#else
#error "radio.cpp：未定义支持的射频芯片宏（需 RADIO_SX126X）"
#endif

static TaskHandle_t g_rf_task = nullptr;
static float g_last_rssi = 0.0f;
static float g_last_snr  = 0.0f;
static bool  g_suspended = false;   // OTA 维护模式：WiFi 与 LoRa 二选一

// DIO1 中断：只发任务通知给 rf_task，不在 ISR 里读 SPI
static void IRAM_ATTR radio_isr() {
    BaseType_t hpw = pdFALSE;
    if (g_rf_task) vTaskNotifyGiveFromISR(g_rf_task, &hpw);
    portYIELD_FROM_ISR(hpw);
}

bool radio_init(float freq, float bw, uint8_t sf, uint8_t cr,
                uint16_t sync, int8_t pwr) {
#if defined(RADIO_SX126X)
    // SPI 引脚重映射（NSS=8/SCK=9/MOSI=10/MISO=11，架构 §0.4）
    SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    g_mod   = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY);
    g_radio = new SX1268(g_mod);

    // 射频开关 active-high（RXEN/TXEN）；禁止 setDio2AsRfSwitchCtrl（DIO2 悬空）
    g_radio->setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);

    // 晶振版无 TCXO：tcxoVoltage=0
    int16_t st = g_radio->begin(freq, bw, sf, cr, sync, pwr, 8, 0.0f);
    if (st != RADIOLIB_ERR_NONE) {
        // 初始化失败：触发射频自救（软复位，累计 3 次整机重启）
        watchdog_rf_fault();
        return false;
    }

    attachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1), radio_isr, RISING);
    return true;
#else
    return false;
#endif
}

static uint8_t g_tx_fail = 0;

int16_t radio_transmit(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (g_suspended) return -1;
#if defined(RADIO_SX126X)
    if (!g_radio) return -1;
    int16_t st = g_radio->transmit(const_cast<uint8_t*>(data), len);
    if (st == RADIOLIB_ERR_NONE) {
        g_tx_fail = 0;
        return 0;
    }
    // 连续 3 次发送失败 -> 触发射频自救（软复位，累计 3 次则整机重启，架构 §8.2）
    if (++g_tx_fail >= 3) {
        g_tx_fail = 0;
        watchdog_rf_fault();
    }
    return st;
#else
    return -1;
#endif
}

bool radio_startReceive() {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio) return false;
    return g_radio->startReceive() == RADIOLIB_ERR_NONE;
#else
    return false;
#endif
}

int16_t radio_readPacket(uint8_t* buf, size_t maxlen, size_t* len,
                         float* rssi, float* snr) {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio) return -1;
    int16_t st = g_radio->readData(buf, maxlen);
    if (st != RADIOLIB_ERR_NONE) return st;
    size_t l = g_radio->getPacketLength();
    if (len) *len = l;
    g_last_rssi = g_radio->getRSSI();
    g_last_snr  = g_radio->getSNR();
    if (rssi) *rssi = g_last_rssi;
    if (snr)  *snr  = g_last_snr;
    return 0;
#else
    return -1;
#endif
}

// CAD 侦听：0=空闲 1=忙 负=错误码
int16_t radio_channelActivity() {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio) return -1;
    int16_t st = g_radio->startChannelScan();
    if (st != RADIOLIB_ERR_NONE) return st;
    int16_t res = g_radio->getChannelScanResult();
    if (res == RADIOLIB_CHANNEL_FREE) return 0;
    if (res == RADIOLIB_PREAMBLE_DETECTED || res == RADIOLIB_LORA_DETECTED) return 1;
    return res;
#else
    return -1;
#endif
}

float radio_lastRssi() { return g_last_rssi; }
float radio_lastSnr()  { return g_last_snr; }

// 运行时切 SF（ADR 用）：先 standby 再改参数再回接收态
bool radio_set_sf(uint8_t sf) {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio) return false;
    int16_t st = g_radio->standby();
    if (st != RADIOLIB_ERR_NONE) return false;
    st = g_radio->setSpreadingFactor(sf);
    if (st != RADIOLIB_ERR_NONE) return false;
    return g_radio->startReceive() == RADIOLIB_ERR_NONE;
#else
    return false;
#endif
}

void radio_hardReset() {
#if defined(RADIO_SX126X)
    if (g_radio) g_radio->reset();
#endif
}

void radio_suspend() {
#if defined(RADIO_SX126X)
    g_suspended = true;
    if (g_radio) g_radio->standby();   // 停收发，释放空口给 WiFi 维护
#endif
}

void radio_resume() {
#if defined(RADIO_SX126X)
    g_suspended = false;
    if (g_radio) g_radio->startReceive();
#endif
}

// ---- rf_task：DIO1 中断 -> 读包 -> 投 q_rf_rx ----
static void rf_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    radio_startReceive();
    for (;;) {
        watchdog_feed();   // 每轮先喂狗：无论收包还是超时都要喂
        // 带 1s 超时等 DIO1 中断：超时也醒来喂狗，避免任务被 WDT 判定卡死
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
            continue;   // 超时（无包）：不读包，直接进入下一轮
        }
        uint8_t buf[FRAME_MTU];
        size_t len = 0;
        float rssi, snr;
        if (radio_readPacket(buf, FRAME_MTU, &len, &rssi, &snr) == 0 && len > 0) {
            queue_item_t item = {};
            memcpy(item.data, buf, len);
            item.len  = (uint16_t)len;
            item.rssi = (int16_t)rssi;
            item.snr  = (int16_t)snr;
            if (xQueueSend(q_rf_rx, &item, 0) != pdTRUE) {
                // 队列满：丢弃（mesh 层会因缺帧自然丢包）
            }
        }
        radio_startReceive();
    }
}

void radio_start(void) {
    xTaskCreatePinnedToCore(rf_task_func, "rf", RF_TASK_STACK,
                            nullptr, RF_TASK_PRIO, &g_rf_task, RF_TASK_CORE);
}
