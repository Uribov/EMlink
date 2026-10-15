# EMlink 任务书 A-6：OTA 回滚确认 + 维护模式超时（对应审查日志 P1-5 的两项）

## 背景（为什么改）

### ① 缺 `esp_ota_mark_app_valid_cancel_rollback()`——真实故障，不是理论问题
已确认本平台 **启用了应用回滚**（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`、`CONFIG_APP_ROLLBACK_ENABLE=y`，见 Arduino-ESP32 2.0.17 的 esp32s3 sdkconfig）。
后果：`ota.cpp:211` 那句 `Update.end(true)` 之后重启进入的新固件处于 **PENDING_VERIFY** 状态；它**从不调用**确认 API，于是**下一次重启时 bootloader 会悄悄把固件回滚到升级前的旧版本**——现场表现为"升完能用，断电重启又变回老版本"。
修法：新固件启动后、确认自己**基本健康**（射频已就绪）时调用 `esp_ota_mark_app_valid_cancel_rollback()`；若射频起不来就不确认，交给 bootloader 回滚（这是一道有用的安全网）。

### ② 维护模式（WiFi AP + 网页 OTA）进来容易出去难
`ota_start()` 开了 AP 就只等 CLI 手动 `wifi off`（`ota.cpp:298`）。AP 一旦开着就是**无鉴权的改参数 + 刷固件入口**（P1-5 的鉴权策略属 C 档产品决策，本次不做），至少要能自己关。
修法：**空闲 5 分钟无客户端 + 30 分钟硬上限**（刷固件过程中不打断）自动退出维护模式，恢复 LoRa。

## 允许改动的文件（其它一律不动）
1. `firmware/src/service/ota.h`
2. `firmware/src/service/ota.cpp`
3. `firmware/src/main.cpp`（只在 `loop()` 里加一行调用）

**不要动**：任何 `.md`、`cli.cpp`、`webui.h`、`radio.*`、`nvs_cfg.*`。

## 1. ota.h：新增一个接口
```c
// 开机自检：系统启动且射频就绪后调用一次 → 向 bootloader 确认"本固件可用"（取消回滚）
// 由 main.cpp 的 loop() 每轮调用（内部自带"只做一次"保护）
void ota_boot_confirm_tick(void);
```

## 2. ota.cpp
### (a) 顶部：include + 常量
- 新增 `#include <esp_ota_ops.h>`
- 新增常量（带注释说明可外场调）：
```c
// 维护模式自动退出（P1-5）：AP 是无鉴权的改参数/刷固件入口，不能长期敞开
#define OTA_AP_IDLE_MS       300000u   // 5 分钟没有客户端连着 → 自动退出
#define OTA_AP_MAX_MS       1800000u   // 30 分钟硬上限（刷固件过程中不生效）
#define OTA_BOOT_CONFIRM_MS   10000u   // 开机满 10s 且射频就绪才确认新固件
```
- 新增 static：
```c
static uint32_t g_ap_start_ms = 0;      // AP 开启时刻
static uint32_t g_last_client_ms = 0;   // 最后一次"有客户端"的时刻
static bool     g_updating = false;     // 正在上传固件（期间不自动退出）
static bool     g_boot_confirmed = false;  // 本次开机是否已确认固件
```

### (b) `ota_start()`：记录起始时刻
在 `WiFi.softAP(...)` 之后（或函数末尾统一）加：
```c
    g_ap_start_ms    = millis();
    g_last_client_ms = millis();
    g_updating       = false;
```

### (c) `ota_task_func()`：循环里加超时判定
在 `while (g_active)` 循环体内（`handleClient()` 之后）加：
```c
        // 维护模式自动退出（P1-5）：客户端在 → 刷新活动时间；刷固件中 → 一律不超时
        if (WiFi.softAPgetStationNum() > 0) g_last_client_ms = millis();
        uint32_t now = millis();
        if (!g_updating) {
            if (now - g_last_client_ms > OTA_AP_IDLE_MS) {
                Serial.println("[ota] 维护模式空闲超时（5 分钟无客户端）→ 自动退出");
                ota_stop();          // 任务内部调用：只置 g_active=false，本循环下一轮退出并自清理
            } else if (now - g_ap_start_ms > OTA_AP_MAX_MS) {
                Serial.println("[ota] 维护模式已达 30 分钟上限 → 自动退出");
                ota_stop();
            }
        }
```
**注意**：`ota_stop()` 内部只置标志位，本任务自己负责清理（现有设计），**不要**在任务里直接删 g_server / 关 WiFi。

### (d) `handle_upload()`：上传期间置位（防止刷固件被超时打断）
- `UPLOAD_FILE_START` 分支：`g_updating = true;`
- `UPLOAD_FILE_END` 分支：处理完（成功/失败都）`g_updating = false;`
- `UPLOAD_FILE_ABORTED`（若原代码有该分支）：也置 false

### (e) 新增 `ota_boot_confirm_tick()`
```c
void ota_boot_confirm_tick(void) {
    if (g_boot_confirmed) return;
    if (millis() < OTA_BOOT_CONFIRM_MS) return;
    if (!radio_is_ready()) return;      // 射频没起来 → 不确认，交给 bootloader 回滚（安全网）
    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    if (r == ESP_OK)                    Serial.println("[ota] 新固件已确认可用（取消回滚）");
    else if (r == ESP_ERR_NOT_SUPPORTED) Serial.println("[ota] 平台未启用回滚，跳过确认");
    else                                Serial.printf("[ota] 固件确认失败: %d\n", (int)r);
    g_boot_confirmed = true;            // 无论成败只做一次（避免刷屏 / 反复调用）
}
```
（`radio.h` 已经 include 在 ota.cpp 中；若没有请补 `#include "../drivers/radio.h"`。）

## 3. main.cpp
`loop()` 里在 `delay(10);` 之前加一行：
```c
    ota_boot_confirm_tick();   // P1-5：系统健康后向 bootloader 确认固件（取消回滚）
```
（`main.cpp` 已 include ota.h；若没有请补。）

## 4. 自检（请在报告里给出结论）
1. 刷完固件重启后，为什么新固件不会被回滚了？如果射频起不来会发生什么？
2. 客户端连着网页但没人操作，最长多久会被踢出维护模式？为什么这么设计？
3. 正在上传固件的过程中，超时逻辑会不会打断它？靠哪个标志位保证？
4. `ota_stop()` 在 ota_task 内部调用是否安全（说明现有清理机制）？

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检的结论
3. 你认为本次改动**不能**保证哪些性质（诚实列出，例如"网页/OTA 仍然没有鉴权"）
