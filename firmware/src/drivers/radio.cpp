// ============================================================
// radio.cpp —— 射频驱动抽象实现（唯一射频入口）
// 上层只认 radio_*，不直接碰 RadioLib。芯片按宏分叉：
//   RADIO_SX126X：SX1261/2/8 + LLCC68
//
// 本板模块 = E220-900M22S = LLCC68 die，用 RadioLib 的 `SX1262` 类驱动。原因：
//   RadioLib 的 begin() 会拿"芯片自报型号"和驱动类声明的名字逐字符比对
//   （lib/radiolib/src/modules/SX126x/SX126x.cpp:2169），对不上直接返回 -2。
//   SX1268 类要求 "SX1268"、SX1262 类要求 "SX1261"（SX1262.h:15 注释：
//   "all SX1262 devices report as SX1261"），而 E220 实测自报 "SX1261"（RadioLib issue #1329）。
// 若上板仍报 -2：依次把下面的 SX1262 换成 LLCC68、再换回 SX1268 试（三者同一套 SPI 命令，
//   切换成本 = 改这一处 + 重编译）。依据见 docs/references/LLCC68_E220-900M22S.md §3。
// ============================================================
#include "radio.h"
#include "../rtos.h"
#include "../service/watchdog.h"
#include "airtime.h"

#if defined(RADIO_SX126X)
static SX1262* g_radio = nullptr;
static Module* g_mod   = nullptr;
#else
#error "radio.cpp：未定义支持的射频芯片宏（需 RADIO_SX126X）"
#endif

static TaskHandle_t g_rf_task = nullptr;
static float g_last_rssi = 0.0f;
static float g_last_snr  = 0.0f;
static bool  g_suspended = false;   // OTA 维护模式：WiFi 与 LoRa 二选一
static uint32_t g_rx_drop = 0;      // 收包队列满丢弃（P1-10）
static uint32_t g_req_drop = 0;     // 射频请求队列满丢弃（P1-10）

// 当前调制参数（CAD 超时推导用；radio_init 设定、radio_set_sf 更新）
static uint8_t g_cur_sf = LORA_DEF_SF;
static float   g_cur_bw_khz = LORA_DEF_BW;
static uint8_t g_cur_cr = LORA_DEF_CR;

// 就绪状态 + 初始化参数（射频失败后 rf_task 每 10s 用这些参数自动重试）
static volatile bool g_radio_ready = false;
static float g_init_freq, g_init_bw;
static uint8_t g_init_sf, g_init_cr;
static uint16_t g_init_sync;
static int8_t g_init_pwr;

static int16_t g_last_err = 0;              // 最近一次 begin() 的错误码
static volatile bool g_ever_ready = false;  // 曾经成功初始化过（区分"热故障"与"根本没有模块"）
static volatile bool g_absent = false;      // 判定无模块：射频已停用
static uint8_t g_init_fails = 0;            // 连续初始化失败次数
static uint32_t g_req_timeout = 0;          // 请求等待超时次数

// 射频请求等待上限：正常操作最长是 200B 满帧发送（SF9/BW125 约 1.7s），3s 留余量
#define RADIO_REQ_TIMEOUT_MS  3000

// ---- 射频单所有者（P0-2）：请求模型（仅本模块内部使用，不进头文件）----
#define RADIO_REQ_HI_LEN 6
#define RADIO_REQ_LO_LEN 4

enum { ROP_TX = 0, ROP_CAD, ROP_SET_SF, ROP_SUSPEND, ROP_RESUME, ROP_RESET };

struct radio_req_t {
    uint8_t  op;               // ROP_*
    uint8_t  sf;               // ROP_SET_SF 用
    uint16_t len;              // ROP_TX 用
    int16_t  result;           // 所有者执行后回填
    TaskHandle_t waiter;       // 等待者任务（调用方填）
    uint8_t  data[FRAME_MTU];  // ROP_TX 的数据（调用方拷入）
};

static QueueHandle_t g_req_hi = nullptr;              // 高优先：控制操作 + ACK/控制帧 TX
static QueueHandle_t g_req_lo = nullptr;              // 低优先：普通数据帧 TX
static TaskHandle_t  g_radio_owner = nullptr;         // 射频所有者（= rf_task）

// DIO1 中断：只发任务通知给 rf_task，不在 ISR 里读 SPI
static void IRAM_ATTR radio_isr() {
    BaseType_t hpw = pdFALSE;
    if (g_rf_task) vTaskNotifyGiveFromISR(g_rf_task, &hpw);
    portYIELD_FROM_ISR(hpw);
}

bool radio_init(float freq, float bw, uint8_t sf, uint8_t cr,
                uint16_t sync, int8_t pwr) {
    // 先存参数（失败后的周期重试据此重新 begin）
    g_init_freq = freq; g_init_bw = bw;
    g_init_sf = sf; g_init_cr = cr;
    g_init_sync = sync; g_init_pwr = pwr;
#if defined(RADIO_SX126X)
    // SPI 引脚重映射（NSS=8/SCK=9/MOSI=10/MISO=11，架构 §0.4）
    SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    g_mod   = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_NRST, PIN_LORA_BUSY);
    g_radio = new SX1262(g_mod);

    // 射频开关 active-high（RXEN/TXEN）；禁止 setDio2AsRfSwitchCtrl（DIO2 悬空）
    g_radio->setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);

    // 晶振版无 TCXO：tcxoVoltage=0
    int16_t st = g_radio->begin(freq, bw, sf, cr, sync, pwr, 8, 0.0f);
    if (st != RADIOLIB_ERR_NONE) {
        // 初始化失败：置未就绪。只有"曾经成功过"才走射频自救——
        // 否则"没插模块"会被 watchdog_rf_fault 放大成整机重启循环。
        g_radio_ready = false;
        g_last_err = st;
        g_init_fails++;
        if (g_ever_ready) watchdog_rf_fault();
        return false;
    }

    // P1-11：RadioLib 的 begin() 会把 OCP 设成 60mA，而 +22dBm 发射需 120~140mA
    // （Semtech 建议 140mA）。功率 > 14dBm 时放宽 OCP，避免高功率发射触发过流保护。
    // 必须在 begin() 之后调用——begin() 内部会把 OCP 设回 60mA。
    if (pwr > 14) g_radio->setCurrentLimit(140.0f);

    attachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1), radio_isr, RISING);
    g_radio_ready = true;
    g_ever_ready  = true;
    g_absent      = false;
    g_init_fails  = 0;
    g_last_err    = 0;
    g_cur_sf = sf;
    g_cur_bw_khz = bw;
    g_cur_cr = cr;
    return true;
#else
    g_radio_ready = false;
    return false;
#endif
}

bool radio_is_ready(void) { return g_radio_ready; }

static uint8_t g_tx_fail = 0;

// ---- 底层动作：所有芯片操作只准在 rf_task（所有者）上下文执行 ----
static int16_t radio_do_tx(const uint8_t* data, size_t len) {
    if (g_suspended || !g_radio_ready) return -1;
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
    if (g_suspended || !g_radio || !g_radio_ready) return false;
    return g_radio->startReceive() == RADIOLIB_ERR_NONE;
#else
    return false;
#endif
}

int16_t radio_readPacket(uint8_t* buf, size_t maxlen, size_t* len,
                         float* rssi, float* snr) {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio || !g_radio_ready) return -1;
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

// CAD 侦听（LBT 用）：状态驱动轮询，直到 CAD 完成或超时（P0-3）
//   0 = 空闲（CAD_DONE）；1 = 忙（CAD_DETECTED）；负 = 错误/超时（调用方按“忙”处理）
static int16_t radio_do_cad() {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio || !g_radio_ready) return -1;

    // 启动 CAD（非阻塞：RadioLib 只把芯片设为 CAD 模式就返回）
    int16_t st = g_radio->startChannelScan();
    if (st != RADIOLIB_ERR_NONE) return st;

    // CAD 硬件耗时 ≈ 若干个符号周期 → 按当前 SF/BW 推导超时（符号时长 us = 2^SF / BW）
    // 5 个符号 + 3ms 余量；轮询期间 vTaskDelay(1) 让出 CPU（rf_task 优先级更高，不受影响）
    uint32_t tsym_us = ((uint32_t)1 << g_cur_sf) * 1000u / (uint32_t)g_cur_bw_khz;   // 例：SF7/125k → 1024us
    uint32_t timeout_ms = tsym_us * 5u / 1000u + 3u;                                  // 例：SF7 → 8ms，SF9 → 23ms
    uint32_t t0 = millis();
    while (millis() - t0 <= timeout_ms) {
        int16_t res = g_radio->getChannelScanResult();
        if (res == RADIOLIB_CHANNEL_FREE) return 0;                                  // CAD_DONE → 空闲
        if (res == RADIOLIB_LORA_DETECTED || res == RADIOLIB_PREAMBLE_DETECTED) return 1;  // CAD_DETECTED → 忙
        // RADIOLIB_ERR_UNKNOWN：CAD 尚未完成 → 继续轮询
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -2;   // 超时：状态未知 → 调用方按“忙”处理
#else
    return -1;
#endif
}

float radio_lastRssi() { return g_last_rssi; }
float radio_lastSnr()  { return g_last_snr; }

// 运行时切 SF（ADR 用）：先 standby 再改参数再回接收态
static bool radio_do_set_sf(uint8_t sf) {
#if defined(RADIO_SX126X)
    if (g_suspended || !g_radio || !g_radio_ready) return false;
    int16_t st = g_radio->standby();
    if (st != RADIOLIB_ERR_NONE) return false;
    st = g_radio->setSpreadingFactor(sf);
    if (st != RADIOLIB_ERR_NONE) return false;
    g_cur_sf = sf;
    return g_radio->startReceive() == RADIOLIB_ERR_NONE;
#else
    return false;
#endif
}

static void radio_do_suspend() {
#if defined(RADIO_SX126X)
    g_suspended = true;
    if (g_radio) g_radio->standby();   // 停收发，释放空口给 WiFi 维护
#endif
}

static void radio_do_resume() {
#if defined(RADIO_SX126X)
    g_suspended = false;
    if (g_radio) g_radio->startReceive();
#endif
}

// ---- 统一入口（归属判定）：所有者上下文直接执行；否则请求队列 + 阻塞等结果 ----
static int16_t radio_exec_inline(uint8_t op, uint8_t sf, const uint8_t* data, size_t len) {
    switch (op) {
        case ROP_TX:      return radio_do_tx(data, len);
        case ROP_CAD:     return radio_do_cad();
        case ROP_SET_SF:  return radio_do_set_sf(sf) ? 0 : -1;
        case ROP_SUSPEND: radio_do_suspend(); return 0;
        case ROP_RESUME:  radio_do_resume();  return 0;
        case ROP_RESET:   if (g_radio) g_radio->reset(); return 0;
        default:          return -1;
    }
}

static int16_t radio_call(uint8_t op, uint8_t sf, const uint8_t* data, size_t len, bool hi) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    // 判定无模块：不排队、不等待（否则调用方会在死掉的射频任务上无限等）
    if (g_absent && op != ROP_SUSPEND && op != ROP_RESUME) return -1;
    if (g_radio_owner == nullptr || g_radio_owner == me) {
        return radio_exec_inline(op, sf, data, len);   // 启动早期 / 所有者自己调用
    }
    if (op == ROP_TX && len > FRAME_MTU) return -1;

    radio_req_t req = {};
    req.op = op; req.sf = sf; req.waiter = me;
    if (op == ROP_TX && data && len > 0) {
        memcpy(req.data, data, len);
        req.len = (uint16_t)len;
    }

    (void)ulTaskNotifyTake(pdTRUE, 0);                 // 清残留通知，防误判完成
    // xQueueSend 按值拷贝整个 req；因此调用方超时返回也不会留下悬垂指针
    QueueHandle_t q = hi ? g_req_hi : g_req_lo;
    if (xQueueSend(q, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_req_drop++;   // 请求队列满：快速失败（P1-10 加计数）
        return -1;
    }
    xTaskNotifyGive(g_radio_owner);                    // 唤醒所有者（与 DIO1 通知共用，多一次无害）
    // 等所有者执行完；超时说明所有者卡住（例如无模块时 begin() 死等），
    // 此时绝不无限等——调用方必须能继续往下走。
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RADIO_REQ_TIMEOUT_MS)) == 0) {
        g_req_timeout++;
        return -1;
    }
    return req.result;
}

// ---- 公开 API：委托给所有者（签名保持不变）----

int16_t radio_transmit(const uint8_t* data, size_t len) {
    return radio_call(ROP_TX, 0, data, len, false);
}

uint32_t radio_airtime_ms(size_t len_bytes) {
    if (!g_radio_ready || g_cur_cr < 5) return 0;
    return airtime_ms((uint32_t)len_bytes, g_cur_sf, g_cur_bw_khz, g_cur_cr);
}

// 高优先发送（ACK/控制帧用；与 radio_transmit 同语义，但排在普通数据帧前面）
int16_t radio_transmit_prio(const uint8_t* data, size_t len) {
    return radio_call(ROP_TX, 0, data, len, true);
}

int16_t radio_channelActivity() {
    return radio_call(ROP_CAD, 0, nullptr, 0, true);
}

bool radio_set_sf(uint8_t sf) {
    return radio_call(ROP_SET_SF, sf, nullptr, 0, true) == 0;
}

void radio_hardReset() {
    (void)radio_call(ROP_RESET, 0, nullptr, 0, true);
}

void radio_suspend() {
    (void)radio_call(ROP_SUSPEND, 0, nullptr, 0, true);
}

void radio_resume() {
    (void)radio_call(ROP_RESUME, 0, nullptr, 0, true);
}

// ---- rf_task：DIO1 中断 -> 读包 -> 投 q_rf_rx ----

#define RADIO_RETRY_MS 10000

// 射频初始化失败后的周期重试（不触发 watchdog_rf_fault，避免"累计 3 次整机重启"被重试放大）
static void radio_retry_tick() {
    if (g_radio_ready || !g_radio || g_suspended || g_absent) return;

    // 从未成功过 → 判定"没有模块"，一次都不自动重试。
    // 实测：开机那一次 begin() 会正常返回失败，但紧接着的重试就可能死等（阻塞 >5 s）
    // → rf_task 不喂狗 → 任务看门狗重启。所以宁可不再重试；
    // 装好模块后由用户 reset（开机重新 init）即可。
    if (!g_ever_ready) {
        g_absent = true;
        Serial.printf("[radio] 未检测到射频模块（err=%d）：射频已停用，"
                      "CLI/WiFi 维护模式可用。检查 E220 排线/供电后敲 reset 重试。\n",
                      (int)g_last_err);
        return;
    }

    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < RADIO_RETRY_MS) return;
    last_ms = now;

    watchdog_feed();   // 进入可能阻塞的 begin() 前先喂一次狗（能减少、但不能消除 begin 内部死等）

    int16_t st = g_radio->begin(g_init_freq, g_init_bw, g_init_sf, g_init_cr, g_init_sync, g_init_pwr, 8, 0.0f);
    if (st == RADIOLIB_ERR_NONE) {
        if (g_init_pwr > 14) g_radio->setCurrentLimit(140.0f);
        attachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1), radio_isr, RISING);
        g_radio_ready = true;
        g_ever_ready  = true;
        g_init_fails  = 0;
        g_last_err    = 0;
        g_cur_sf = g_init_sf;
        g_cur_bw_khz = g_init_bw;
        g_cur_cr = g_init_cr;
        Serial.println("[radio] 初始化重试成功，射频已恢复");
    } else {
        g_last_err = st;
        if (!g_ever_ready) {
            // 正常不会走到这里（从未成功过的分支在函数开头就 return 了），保留以防御意外路径
            g_init_fails++;
            Serial.printf("[radio] 初始化失败（err=%d）\n", (int)st);
        } else {
            Serial.printf("[radio] 初始化重试失败（%d），%dms 后再试\n", (int)st, RADIO_RETRY_MS);
        }
    }
}

static void rf_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    radio_startReceive();
    for (;;) {
        watchdog_feed();
        radio_retry_tick();

        // 1) 先服务请求（hi 优先，非阻塞）——服务完立刻 continue 再查
        // 队列元素是结构体副本（值语义），这里拿到的是本任务栈上的独立拷贝。
        // 注意：radio_req_t 含 data[FRAME_MTU]（~210B），必须在循环体内声明而不要提到函数顶部，
        // 否则这份栈空间会一直占用 rf_task 栈。
        radio_req_t req;
        if (xQueueReceive(g_req_hi, &req, 0) == pdTRUE) {
            req.result = radio_exec_inline(req.op, req.sf, req.data, req.len);
            if (req.waiter) xTaskNotifyGive(req.waiter);
            continue;
        }
        if (xQueueReceive(g_req_lo, &req, 0) == pdTRUE) {
            req.result = radio_exec_inline(req.op, req.sf, req.data, req.len);
            if (req.waiter) xTaskNotifyGive(req.waiter);
            continue;
        }

        // 2) 无请求 → 等 DIO1 中断（1s 超时）或"有请求"的唤醒（两者共用任务通知）
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
            continue;   // 超时：回循环，服务请求 + 喂狗
        }

        // 3) 醒来：可能是收包完成，也可能是请求唤醒 → 先尝试读包（读不到就当无事发生）
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
                g_rx_drop++;   // 队列满：丢弃（P1-10 加计数，原来完全静默）
            }
        }
        radio_startReceive();
    }
}

void radio_start(void) {
    // 队列与所有者标记必须在任务创建前就位：否则 adr_init 等启动期调用可能走"无所有者→inline"路径
    // 队列元素 = 请求结构体本身（值拷贝），不是指针。
    // 指针语义会在"调用方超时返回、栈帧销毁"后变成悬垂指针 → rf_task 写它 = use-after-free（实测崩溃）。
    // 代价：每个队列槽 ~200B（radio_req_t 含 data[FRAME_MTU]），hi6+lo4 ≈ 2KB RAM。
    g_req_hi = xQueueCreate(RADIO_REQ_HI_LEN, sizeof(radio_req_t));
    g_req_lo = xQueueCreate(RADIO_REQ_LO_LEN, sizeof(radio_req_t));
    xTaskCreatePinnedToCore(rf_task_func, "rf", RF_TASK_STACK,
                            nullptr, RF_TASK_PRIO, &g_rf_task, RF_TASK_CORE);
    g_radio_owner = g_rf_task;   // 从此刻起：射频单所有者 = rf_task
}

int16_t radio_last_error(void)          { return g_last_err; }
bool    radio_absent(void)              { return g_absent; }
uint8_t radio_init_fail_count(void)     { return g_init_fails; }
uint32_t radio_req_timeout_count(void)  { return g_req_timeout; }

uint32_t radio_rx_drop_count(void)  { return g_rx_drop; }
uint32_t radio_req_drop_count(void) { return g_req_drop; }
