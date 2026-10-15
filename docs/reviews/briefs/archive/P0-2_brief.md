# EMlink M0 修复 4/4：射频单所有者（P0-2，架构正解方案）

## 背景
射频/SPI 现在被 **4 条执行路径并发使用**：
- `rf_task`（收包、startReceive）
- `mac_task`（transmit / CAD / set_sf）
- `mesh_task`（**直接**发 ACK，绕过 mac）
- `ota_task`（core1，suspend/resume）

RadioLib/SPI 都不是线程安全的 → 可能出现 SPI 竞态（读回错数据 / 状态机错乱 / 挂死）。
本方案落地**射频单所有者**：**所有芯片操作只准在 `rf_task` 上下文执行**，其它任务只能通过"请求队列 + 通知"委托执行。

## 只允许修改这 3 个文件
1. `firmware/src/drivers/radio.cpp`（主体）
2. `firmware/src/drivers/radio.h`（加 1 个新函数声明 + 注释约定）
3. `firmware/src/link/mac.cpp`（`mac_send_ack` 改用高优先发送接口）

---

## 实现规格（严格按此实现；不要自由发挥）

### 1) 请求模型（**只放 radio.cpp 内部 static，不进头文件**）
```c
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
```
队列元素是 **`radio_req_t*` 指针**；请求结构由调用方放在自己栈上，且调用方会阻塞到执行完成 → 指针生命周期安全。

### 2) 底层动作：把现有公开函数体**原样**抽成内部函数（只改名/挪位置，逻辑一字不改）
- `static int16_t radio_do_tx(const uint8_t* data, size_t len)` ← 现 `radio_transmit()` 的函数体
- `static int16_t radio_do_cad()` ← 现 `radio_channelActivity()` 的函数体（**注意：这是刚修好的"状态驱动轮询"版本，原样搬**）
- `static bool radio_do_set_sf(uint8_t sf)` ← 现 `radio_set_sf()` 的函数体
- `static void radio_do_suspend()` ← 现 `radio_suspend()` 的函数体
- `static void radio_do_resume()` ← 现 `radio_resume()` 的函数体
- ROP_RESET 直接 `g_radio->reset()`（一行，不用单独函数）

### 3) 统一入口（归属判定）
```c
// 所有者上下文（或还没有所有者时，如启动早期）→ 直接执行；否则 → 请求队列 + 阻塞等结果
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
    QueueHandle_t q = hi ? g_req_hi : g_req_lo;
    if (xQueueSend(q, &req, pdMS_TO_TICKS(100)) != pdTRUE) return -1;   // 队列满：快速失败
    xTaskNotifyGive(g_radio_owner);                    // 唤醒所有者（与 DIO1 通知共用，多一次无害）
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // 等所有者执行完（WDT 是兜底）
    return req.result;
}
```

### 4) 公开 API 改为调用 radio_call（**签名保持不变**）
- `radio_transmit(data,len,timeout_ms)` → `radio_call(ROP_TX, 0, data, len, false)`（保留 `(void)timeout_ms;`）
- **新增** `int16_t radio_transmit_prio(const uint8_t* data, size_t len)` → `radio_call(ROP_TX, 0, data, len, true)`
- `radio_channelActivity()` → `radio_call(ROP_CAD, 0, nullptr, 0, true)`
- `radio_set_sf(sf)` → `radio_call(ROP_SET_SF, sf, nullptr, 0, true) == 0`
- `radio_suspend()` → `(void)radio_call(ROP_SUSPEND, 0, nullptr, 0, true)`
- `radio_resume()` → `(void)radio_call(ROP_RESUME, 0, nullptr, 0, true)`
- `radio_hardReset()` → `(void)radio_call(ROP_RESET, 0, nullptr, 0, true)`
- **保持原样不动**：`radio_init`（启动期由 loopTask 或所有者调用）、`radio_startReceive` / `radio_readPacket`（只有 rf_task 用）、`radio_lastRssi` / `radio_lastSnr` / `radio_is_ready`（只读）

### 5) `radio_start()`：先建队列与所有者标记，再创建任务
```c
void radio_start(void) {
    // 队列与所有者标记必须在任务创建前就位：否则 adr_init 等启动期调用可能走"无所有者→inline"路径
    g_req_hi = xQueueCreate(RADIO_REQ_HI_LEN, sizeof(radio_req_t*));
    g_req_lo = xQueueCreate(RADIO_REQ_LO_LEN, sizeof(radio_req_t*));
    xTaskCreatePinnedToCore(rf_task_func, "rf", RF_TASK_STACK,
                            nullptr, RF_TASK_PRIO, &g_rf_task, RF_TASK_CORE);
    g_radio_owner = g_rf_task;   // 从此刻起：射频单所有者 = rf_task
}
```

### 6) `rf_task_func` 改为：每轮先服务请求（hi → lo），再等 DIO1/唤醒
```c
static void rf_task_func(void* arg) {
    (void)arg;
    watchdog_register_current();
    radio_startReceive();
    for (;;) {
        watchdog_feed();
        radio_retry_tick();

        // 1) 先服务请求（hi 优先，非阻塞）——服务完立刻 continue 再查
        radio_req_t* req = nullptr;
        if (xQueueReceive(g_req_hi, &req, 0) == pdTRUE) {
            req->result = radio_exec_inline(req->op, req->sf, req->data, req->len);
            if (req->waiter) xTaskNotifyGive(req->waiter);
            continue;
        }
        if (xQueueReceive(g_req_lo, &req, 0) == pdTRUE) {
            req->result = radio_exec_inline(req->op, req->sf, req->data, req->len);
            if (req->waiter) xTaskNotifyGive(req->waiter);
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
                // 队列满：丢弃（mesh 层会因缺帧自然丢包）
            }
        }
        radio_startReceive();
    }
}
```

### 7) radio.h：新增声明 + 契约注释
```c
// 高优先发送（ACK/控制帧用；与 radio_transmit 同语义，但排在普通数据帧前面）
// 注意：P0-2 起射频归 rf_task 单所有者——以上所有射频接口内部改为"请求队列 + 阻塞等结果"，
// 调用者（mac/mesh/ota/watchdog 等）不需要改调用方式；但调用这些接口的任务
// 不得把"任务通知（task notification）"用于其它用途。
int16_t radio_transmit_prio(const uint8_t* data, size_t len);
```

### 8) `mac.cpp`：ACK 走高优先通道
`mac_send_ack()` 里的 `radio_transmit(buf, n, 500)` 改成 `radio_transmit_prio(buf, n)`。
（原因：mac_task 可能正阻塞在自己等 ACK 的窗口里，ACK 若排在普通数据帧后面会错过 300ms ACK 窗口。）

## 硬约束
- **不要改任何行为语义**：返回值、短路条件（g_suspended/g_radio_ready）、`g_tx_fail` 计数、`watchdog_rf_fault` 触发条件、日志文案，一律保持原样；
- P0-3 的 CAD 轮询实现、P0-4 的 `g_radio_ready` / `radio_retry_tick` 逻辑**原样保留**（只搬家不改逻辑）；
- 不动 `radio_init` / `radio_startReceive` / `radio_readPacket` / `radio_lastRssi/Snr` 的实现；
- 不改其它任何文件（mac.cpp 只动那一行）；不改 .md；
- 中文注释，风格一致；最小 diff；
- **不要运行编译命令**、**不要 git commit**。

## 完成后请输出
1. `git diff --stat`；
2. 5~8 行中文总结。
