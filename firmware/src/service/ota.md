# ota —— OTA 固件升级 / WiFi Web 管理

## 职责
落地维护时的无线管理入口：板子开一个 WiFi 热点，手机连上后打开网页，就能**改参数、设加密密钥、刷固件**，不用插 USB 线。

学 ELRS 的做法：WiFi AP 模式 + 内嵌网页 + 浏览器上传 .bin 刷固件。

## 状态
**已实现**。默认不启动；CLI 敲 `wifi` 触发。

## 依赖
- `WiFi.h` / `WebServer.h` / `Update.h`（ESP32 Arduino 内置，无需额外库）
- `nvs_cfg.h`（读写参数/密钥/node_id/wifi 配置）、`crypto.h`（设密钥）
- `radio.h`（`radio_suspend`/`radio_resume`，维护模式二选一）

## 方案（定死）
| 项 | 值 |
|---|---|
| 模式 | WiFi AP（自己当热点，不连外网） |
| SSID | `EMlink-<node_id>`（可用 `wifi_ssid` 覆盖） |
| IP | 192.168.4.1 |
| 密码 | 默认无密码（`wifi_pass` 可设） |
| 网页功能 | 改参数 / 设密钥 / 刷固件(Update.h) / 退出维护模式 |
| 触发 | CLI `wifi`；退出 `wifi off` 或网页「退出」按钮 |

## 关键设计：WiFi 与 LoRa 二选一
- WiFi 栈默认占 core0，而射频收发任务也全在 core0，同时开会抢 CPU
- 故「维护模式」：`ota_start` 先 `radio_suspend()`（射频 standby + 所有射频操作 no-op），退出时 `radio_resume()`
- 好处：adr/mac/mesh 模块**零改动**——射频 suspend 后它们的操作自动变无害空操作

## OTA 双分区（partitions_ota.csv）
4MB Flash：`app0`/`app1` 各 1.5M + `otadata`（回滚）+ `nvs` + `coredump`，不加 spiffs（网页内嵌固件）。
刷固件写空闲分区，重启后由 otadata 切到新分区，失败可回滚。

## 对外接口
见 `ota.h`：`ota_start / ota_stop / ota_active / ota_handle`。

## 验收
- CLI `wifi` → 手机搜到 `EMlink-<id>` 热点 → 浏览器 192.168.4.1 打开配置页
- 网页改 sf 保存 → 重启后 `get sf` 读到新值
- 网页设 32 hex 密钥 → `crypto` 显示「开」
- 网页上传 .bin → 刷写 → 自动重启 → 版本更新
- 网页「退出」→ LoRa 恢复，`stats` 显示链路正常

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_server`：`WebServer*`（端口 80）
- `g_ota_task`：ota_task 句柄（core1，优先级 6，栈 8KB）
- `g_active`：维护模式标志
- `g_node_id`：SSID 用的节点号（ota_start 时从 NVS 读）

### 任务
- `ota_task`（core1 prio6）：循环 `g_server->handleClient()`，10ms 一次
- 跑 core1 而非 core0：WiFi 栈占 core0，射频也 core0，WebServer 放 core1 不抢

### 路由表（WebServer）
| 路径 | 方法 | 作用 |
|---|---|---|
| `/` | GET | 主页：改参数 + 设密钥 + 刷固件 + 退出 四块表单 |
| `/set` | GET | 读 freq/sf/bw/cr/power 参数写 NVS |
| `/crypto` | GET | `?key=<32hex>` 设密钥；`?off=1` 关加密 |
| `/update` | POST | multipart 上传 .bin，`Update.h` 写 ota_1 后重启 |
| `/exit` | GET | 退出维护模式（`ota_stop`） |

### 内嵌 HTML
- `PAGE_HEAD` / `PAGE_TAIL` 用 `PROGMEM` 存 flash（不占 RAM，也不用 spiffs）
- 主页头部动态拼一行「节点 id / 角色 / 加密状态」（`snprintf` 从 NVS 读）

### 刷固件（handle_update，标准 Update.h 流程）
- `UPLOAD_FILE_START` → `Update.begin(UPDATE_SIZE_UNKNOWN)`
- `UPLOAD_FILE_WRITE` → `Update.write(buf, size)`（分块写 flash）
- `UPLOAD_FILE_END` → `Update.end(true)` → `ESP.restart()`
- 靠 otadata 自动切新分区；写坏可回滚

### 维护模式进出
- 进：`radio_suspend()` → `WiFi.mode(WIFI_AP)` → `softAP(ssid[, pass])` → `softAPConfig(192.168.4.1)` → `WebServer` 起路由 → 起 ota_task
- 出：删任务 → 停/删 WebServer → `WiFi.mode(WIFI_OFF)` → `radio_resume()`

### 关键实现细节
1. **发送响应用手写 HTTP 头 + `client().write`**：内嵌 HTML 用 PROGMEM，直接 `send()` 传 PROGMEM 指针会乱码，故 `ws_send_p` 逐字节 `pgm_read_byte` 写出。
2. **射频 suspend 后 no-op**：`radio_transmit/readPacket/startReceive/set_sf/channelActivity` 开头都查 `g_suspended`，暂停期上层模块照常跑但都不碰 SPI/芯片。
3. **nvs_cfg 新增字段**：`wifi_ssid[32]`/`wifi_pass[32]`/`ota_url[96]`；SSID 空则默认 `EMlink-<node_id>`，pass 空则开放热点，ota_url 预留。
