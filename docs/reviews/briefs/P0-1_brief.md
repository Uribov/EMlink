# EMlink M0 修复 1/4：看门狗接线（P0-1 · 方案 A）

## 背景（务必先理解，再动手）
ESP32-S3 的任务看门狗（TWDT）当前被这样用：

- `watchdog_init()` 里 `esp_task_wdt_add(nullptr)` 把 **loopTask**（Arduino 的 setup/loop 任务）订阅进 WDT；
- 但 loopTask **从不喂狗**（Arduino 框架只有调用 `enableLoopWDT()` 后才会在 loop 里喂，工程里没有这个调用）；
- 真正调用 `watchdog_feed()` 的 `stats_task` 又**没有订阅**，而 IDF 规定 `esp_task_wdt_reset()` 只对"已订阅的当前任务"生效 → 喂不动 loopTask。

结果：5 秒超时 → panic 复位 → **上板就是 5 秒重启循环**。

方案 A（本次要实现的）：
- **不再订阅 loopTask**；
- 由 5 个关键任务**各自订阅、各自喂**：`rf_task`、`mac_task`、`mesh_task`、`mavlink_task`、`stats_task`。

## 只允许修改这 7 个文件
1. `firmware/src/service/watchdog.h`
2. `firmware/src/service/watchdog.cpp`
3. `firmware/src/drivers/radio.cpp`        （只改 `rf_task_func`）
4. `firmware/src/link/mac.cpp`             （只改 `mac_task_func` 与 `mac_send` 的 ACK 等待循环）
5. `firmware/src/net/mesh.cpp`             （只改 `mesh_task_func`）
6. `firmware/src/service/mav_router.cpp`   （只改 `mavlink_task_func`）
7. `firmware/src/app/stats.cpp`            （只改 `stats_task_func`）

## 具体要求

### A. watchdog.h / watchdog.cpp
- `watchdog_init()`：
  - 保留 `esp_task_wdt_init(5, true)`（IDF 4.4 语义：已初始化时会更新超时/panic 配置并返回 ESP_OK，安全）；
  - **删除 `esp_task_wdt_add(nullptr)`**；
  - 注释改为：本函数只做初始化，不订阅任何任务；需要被监视的任务自己调用 `watchdog_register_current()`。
- 新增接口 `void watchdog_register_current(void);`
  - 实现：`esp_task_wdt_add(nullptr)`（订阅当前任务）；返回 `ESP_ERR_INVALID_ARG`（已订阅过）或 `ESP_ERR_INVALID_STATE`（未初始化）都**静默忽略**（不返回错误、不打印）；
  - 注释：调用后必须在该任务循环里周期调用 `watchdog_feed()`。
- `watchdog_feed()`：实现不变（`esp_task_wdt_reset()`），注释更新为"喂的是**当前任务**，必须由已订阅的任务自己调用"。
- `watchdog_rf_fault()` 与统计接口：**不要动**。

### B. `radio.cpp` —— `rf_task_func`
- 任务开头调用 `watchdog_register_current();`
- 主循环原来是无超时死等：`ulTaskNotifyTake(pdTRUE, portMAX_DELAY);`
  → 改成带 1 秒超时：`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))`
  → **返回 0（超时）时不要读包**（`radio_readPacket` 只在收到通知时执行），直接进入下一轮（等于"没包也醒来喂狗"）。
- **每轮循环开头**都调用 `watchdog_feed();`（无论收到包还是超时）。
- 保持原有行为：收包后 `radio_startReceive()` 恢复接收态的逻辑不变。

### C. `mac.cpp`
- `mac_task_func` 开头 `watchdog_register_current();`；循环体每轮调用一次 `watchdog_feed();`
- `mac_send()` 里"等 ACK"的轮询循环：
  ```cpp
  while (!g_ack_arrived && (millis() - start < g_ack_timeout_ms)) {
      vTaskDelay(pdMS_TO_TICKS(5));
  }
  ```
  在循环体内加一行 `watchdog_feed();`（注释：mac_send 只由 mac_task 调用，属已订阅任务；多次重传累计可能超过 WDT 超时）。
- 不要改 LBT / 占空比 / 重传逻辑本身。

### D. `mesh.cpp` —— `mesh_task_func`
- 开头 `watchdog_register_current();`
- 循环体每轮 `watchdog_feed();`（**包括** `xQueueReceive` 100ms 超时走 `frag_tick(); continue;` 的那条分支）。

### E. `mav_router.cpp` —— `mavlink_task_func`
- 开头 `watchdog_register_current();`；循环体每轮 `watchdog_feed();`

### F. `stats.cpp` —— `stats_task_func`
- 开头 `watchdog_register_current();`；保留现有的每轮 `watchdog_feed();`

## 硬约束（违反即返工）
- 只改上面列出的文件与函数；**不要**改 `main.cpp` / `rtos.*` / 任何 `.h` 的其它声明 / 任何 `.md`；
- 不要顺手"优化"别的代码，不要重命名函数，不要加新任务；
- 注释用中文，风格与现有代码一致（一两句讲清"为什么"）；
- **不要运行编译命令**（验证由调用方做）；**不要 git commit**；
- 每处改动保持最小 diff。

## 完成后请输出
1. `git diff --stat` 的结果；
2. 3~6 行中文总结：改了哪些文件、每个文件改了什么。
