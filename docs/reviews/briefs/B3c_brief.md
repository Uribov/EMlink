# EMlink 任务书 B-3c：各层丢弃计数 + 链路质量实现（对应审查日志 P1-10 第 2、3 条）

## 背景（为什么改）
- **队列满全部静默丢、没有任何计数**：`rtos_tx_enqueue()` 的返回值没人检查；`radio.cpp` 收包队列满、`uart_fc`/`usb_cdc` 收队列满也都被写成空处理。现场**无法诊断丢包发生在哪一层**。
- `stats_collect()` 的 `link_quality_pct` **恒为 0**（占位没实现）。

修法：每个丢弃点加计数器 + 通过 CLI `status` 暴露；`link_quality_pct` 用 ACK 成功率算出真实值。

## 允许改动的文件（其它一律不动）
1. `firmware/src/rtos.h`、`firmware/src/rtos.cpp`
2. `firmware/src/drivers/radio.cpp`、`firmware/src/drivers/radio.h`
3. `firmware/src/drivers/uart_fc.cpp`、`firmware/src/drivers/uart_fc.h`
4. `firmware/src/drivers/usb_cdc.cpp`、`firmware/src/drivers/usb_cdc.h`
5. `firmware/src/app/stats.cpp`
6. `firmware/src/app/cli.cpp`

**不要动**：任何 `.md`、`mesh.*`、`mav_router.*`、`mac.*`、`nvs_cfg.*`、`main.cpp`；不要跑 pio/编译；不要 git commit/add。

## 1. rtos：发送队列丢弃计数
`rtos.h` 新增声明：
```c
// 发送队列满导致的丢弃（P1-10：原来静默丢，无计数）
void     rtos_tx_drop_note(void);
uint32_t rtos_tx_drop_count(void);
```
并把 `rtos_tx_enqueue()`（现在是 `static inline`，约 rtos.h:103）改成**先取值、失败计数**（原来的三个 `return` 短路写法改成一次性判断，行为等价）：
```c
static inline bool rtos_tx_enqueue(const queue_item_t* item) {
    if (!item) return false;
    bool ok = (item->qos >= QOS2) ? (xQueueSend(q_mac_tx_hi,  item, 0) == pdTRUE)
            : (item->qos == QOS1) ? (xQueueSend(q_mac_tx_med, item, 0) == pdTRUE)
                                  : (xQueueSend(q_mac_tx_lo,  item, 0) == pdTRUE);
    if (!ok) rtos_tx_drop_note();
    return ok;
}
```
`rtos.cpp` 新增 `static uint32_t g_tx_drop = 0;` + 上面两个函数的实现（自增 / 只读；与其它统计一致：不加锁，近似值可接受）。

## 2. radio：两处丢弃计数
- 新增两个 static：`g_rx_drop`（收包队列满）、`g_req_drop`（请求队列满）
- 收包路径（约 radio.cpp:348，`rf_task` 里）现在是**空分支**：
```c
            if (xQueueSend(q_rf_rx, &item, 0) != pdTRUE) {
                g_rx_drop++;   // 队列满：丢弃（P1-10 加计数，原来完全静默）
            }
```
- 请求路径（约 radio.cpp:244，`radio_call()` 里）：
```c
    if (xQueueSend(q, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_req_drop++;   // 请求队列满：快速失败（P1-10 加计数）
        return -1;
    }
```
- `radio.h` 声明：
```c
uint32_t radio_rx_drop_count(void);    // 收包队列满丢弃数（P1-10）
uint32_t radio_req_drop_count(void);   // 射频请求队列满丢弃数（P1-10）
```
- `radio.cpp` 末尾加两个访问器实现。

## 3. uart_fc / usb_cdc：收队列丢弃计数
各自文件内 `static uint32_t g_rx_drop = 0;`，把"收到的字节块入队"那一句改成检查返回值：
- `uart_fc.cpp`（约 40 行）：
```c
            if (xQueueSend(q_uart_rx, &item, 0) != pdTRUE) g_rx_drop++;   // 队列满丢弃（P1-10）
```
- `usb_cdc.cpp`（约 37 行）：同样改成对 `q_usb_rx` 判断
- 各自 `.h` 声明 + `.cpp` 实现访问器：
```c
uint32_t uart_fc_drop_count(void);   // 飞控上行队列满丢弃数（P1-10）
uint32_t usb_cdc_drop_count(void);   // 地面站下行队列满丢弃数（P1-10）
```

## 4. stats：把 link_quality_pct 算出来
`stats_collect()` 里现在这句是占位：
```c
    out->link_quality_pct = 0;
```
改成按 **ACK 成功率**（最能反映"这条链路还能不能可靠送达"；纯收包数无法区分"没发"和"发了没人应"）：
```c
    // 链路质量 = 发出去的帧里拿到 ACK 的比例（P1-10；原为恒 0 的占位）
    uint32_t ack_total = out->ack_ok + out->ack_miss;
    out->link_quality_pct = ack_total ? (100u * out->ack_ok / ack_total) : 0;
```
（`link_quality_pct` 的字段类型见 `stats.h`，按它做转换；`0` 语义 = 还没有 ACK 样本。）

## 5. cli：status 增加丢包行
在现有「ADR」行之后追加（status 由十一行变十二行）：
```c
        snprintf(line, sizeof(line), "丢包: 收包队列=%u 射频请求=%u 发送队列=%u UART=%u USB=%u\r\n",
                 (unsigned)radio_rx_drop_count(), (unsigned)radio_req_drop_count(),
                 (unsigned)rtos_tx_drop_count(), (unsigned)uart_fc_drop_count(),
                 (unsigned)usb_cdc_drop_count());
        cli_print(line);
```
（`rtos.h` / `../drivers/radio.h` / `../drivers/uart_fc.h` / `../drivers/usb_cdc.h` 若未 include 请补。）

## 6. 自检（报告里给结论）
1. 这些计数为什么可以不加锁？（说明各丢弃点分别跑在哪个任务上下文）
2. `rtos_tx_enqueue` 改成"先算 bool 再计数"后，行为与原来的三个 `return` 分支**是否完全等价**？
3. `link_quality_pct` 为什么用 ACK 成功率而不是收包计数？
4. 这些计数现在通过什么暴露给用户？为什么**不**顺手往 `stats_t` 里加字段？

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检结论
3. 本次改动**不能**保证哪些性质
