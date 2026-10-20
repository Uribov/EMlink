# ota —— OTA 固件升级 / WiFi Web 管理

## 职责

落地维护时的无线管理入口：板子开一个 WiFi 热点，手机连上后打开网页，就能改参数、设加密密钥、刷固件，不用插 USB 线。并且负责向 bootloader 确认新固件可用（取消回滚）。

它是设备在野外唯一的免拆机入口，所以自动进入和自动退出两套逻辑都在这里。


## 状态

已实现。默认不启动；CLI 敲 `wifi` 触发。


## 依赖

- `WiFi.h` / `WebServer.h` / `Update.h` / `esp_ota_ops.h`（ESP32 Arduino / IDF 内置，无需额外库）
- `nvs_cfg.h`（读写参数/密钥/node_id/wifi 配置）、`crypto.h`（设密钥）
- `radio.h`（`radio_suspend`/`radio_resume` 二选一；`radio_is_ready` 判固件确认时机）
- `webui.h`（页面静态资源：HTML 骨架 + CSS + 几何标记，仅本模块用）

依赖全是框架内置件，没有额外引入的库。


## 方案（定死）

| 项 | 值 |
|---|---|
| 模式 | WiFi AP（自己当热点，不连外网） |
| SSID | `EMlink-<node_id>`（可用 `wifi_ssid` 覆盖） |
| IP | 192.168.4.1 |
| 密码 | 默认无密码（`wifi_pass` 可设） |
| 网页功能 | 改参数（含 节点号 / 角色 / 自动维护延时，S6）/ 设密钥 / 刷固件(Update.h) / 恢复出厂 / 退出维护模式；「保存并重启」按钮在同一表单里提交 `reboot=1`，保存成功 → 回执 → `ESP.restart()` |
| 网页风格 | 学 ELRS：深色底、大标题、纯黑白灰、1px 发丝线、零圆角、等边三角网格标记 |
| 触发 | CLI `wifi`；退出 `wifi off` 或网页「退出」按钮；上电满 `auto_wifi_s`（默认 30s，15–60）且无链路活动 → 自动进入（S5） |
| 自动退出 | 空闲 5 分钟无客户端 或 30 分钟硬上限（刷固件中不生效）（P1-5） |
| 状态灯 | 维护模式期间 GPIO48 跑彩虹灯：21 档色表（7 基色 + 每两档之间 2 个插值色）+ 档间连续插值，2.5 秒一轮，亮度 60（S9–S11）。S14 起 LED 由独立任务驱动（20ms，core1），见 `stats.cpp::stats_led_tick()` + `led_states.md` |

页面视觉学 ELRS 但代码自己写：ELRS 的实现是 Vue SPA（压缩后 250–320KB），本工程既放不下也不需要。


## 关键设计：WiFi 与 LoRa 二选一

- WiFi 栈默认占 core0，而射频收发任务也全在 core0，同时开会抢 CPU
- 故「维护模式」：`ota_start` 先 `radio_suspend()`（射频 standby + 所有射频操作 no-op），退出时 `radio_resume()`
- 好处：adr/mac/mesh 模块零改动 —— 射频 suspend 后它们的操作自动变无害空操作

用「暂停射频」换「不抢 CPU」，代价是维护期间 LoRa 不通。


## OTA 双分区（partitions_ota.csv）与回滚确认（P1-5）

4MB Flash：`app0`/`app1` 各 1.5M + `otadata`（回滚）+ `nvs` + `coredump`，不加 spiffs（网页内嵌固件）。
刷固件写空闲分区，重启后由 otadata 切到新分区。

本平台启用了应用回滚（Arduino-ESP32 2.0.17 的 sdkconfig：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`、`CONFIG_APP_ROLLBACK_ENABLE=y`）。
因此刷完固件重启进入的新固件处于 PENDING_VERIFY：必须调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认，否则下次重启 bootloader 会默默回滚到升级前的旧版本（现场表现：「升完能用，断电重启又变回老固件」）。
本模块的做法：开机满 `OTA_BOOT_CONFIRM_MS`（10s）且 `radio_is_ready()` 为真 → 确认一次。

- 射频一直起不来 → 不确认 → 下次重启回滚到旧固件（安全网：宁可回退，也不把半瘫固件固化）
- 代价：若射频起不来是「配置写错」而非固件问题，回滚也救不了（要连 USB 用 CLI 改回参数）

确认条件绑在「射频就绪」上，等于用射频能不能起来当固件好坏的判据。


## 对外接口

见 `ota.h`：`ota_start / ota_stop / ota_active`（WebServer 轮询在 ota_task 内部，无对外单步接口）+ `ota_boot_confirm_tick`（main 的 loop 每轮调；开机自检）+ `ota_auto_tick`（main 的 loop 每轮调；上电自动进维护模式，S5/2026-10-17）。

对外三个控制函数加两个 loop 里调的 tick。


## 验收

- CLI `wifi` → 手机搜到 `EMlink-<id>` 热点 → 浏览器 192.168.4.1 打开配置页
- 网页改 sf 保存 → 重启后 `get sf` 读到新值
- 网页设 32 hex 密钥 → `crypto` 显示「开」
- 网页密钥填不足 32 位（或含非 hex 字符）→ 报「请输入正确的格式」，不提交、不改动原密钥
- 网页密钥里粘贴时带了空格/换行 → 自动清掉后仍判定通过
- 网页上传 .bin → 刷写 → 自动重启 → 串口打印「新固件已确认可用（取消回滚）」→ 再断电重启一次，仍是新版本（P1-5 关键验收）
- 网页上传时没选文件 → 立刻回「刷写失败」，不转圈
- 网页「退出」→ LoRa 恢复，`stats` 显示链路正常
- AP 开着无人连 → 5 分钟后自动退出、LoRa 恢复；挂着客户端不动 → 30 分钟自动退出（P1-5）

最后两条验超时退出，倒数第四条是 OTA 回滚确认的关键验收。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

维护模式入口：开 WiFi 热点 + 内嵌网页，改参数/设密钥/刷固件都在浏览器里做；同时负责向 bootloader 确认新固件可用，否则下次重启会被回滚。


### static 变量

- `g_server`：`WebServer*`（端口 80），由 ota_task 唯一持有/删除
- `g_ota_task`：ota_task 句柄（core1，优先级 6，栈 8KB）
- `g_active`：维护模式标志（`volatile`，ota_stop 只置 false）
- `g_node_id`：SSID 用的节点号（ota_start 时从 NVS 读）
- `g_upd_ok`：本次刷写是否成功；`/update` 的完成回调靠它区分「成功（已回过话并重启）」和「失败/压根没选文件」
- `g_ap_start_ms` / `g_last_client_ms` / `g_updating`：维护模式超时判定（P1-5）；`g_updating` 由 `handle_upload` 的 START/END 置位
- `g_boot_confirmed`：本次开机是否已向 bootloader 确认固件（只做一次）

前四个是运行状态，后三个分别管超时判定和开机确认。


### 常量（编译期，外场要调就重编译）

- `OTA_AP_IDLE_MS=300000`（5 分钟无客户端 → 自动退出）
- `OTA_AP_MAX_MS=1800000`（30 分钟硬上限；客户端挂着不走的兜底）
- `OTA_BOOT_CONFIRM_MS=10000`（开机 10s 后才可能确认固件）

三个超时常量分两级：空闲退出、硬上限退场、开机确认延时。


### 任务

- `ota_task`（core1 prio6）：`while (g_active)` 循环 `g_server->handleClient()` + 超时判定；g_active 变 false 后自行清理（停/删 g_server、关 WiFi、radio_resume）再 `vTaskDelete(nullptr)` 删自己
- 跑 core1 而非 core0：WiFi 栈占 core0，射频也 core0，WebServer 放 core1 不抢

任务自己删自己是刻意设计，避免从别的上下文删掉它导致清理代码不执行。


### 路由表（WebServer）

| 路径 | 方法 | 作用 |
|---|---|---|
| `/` | GET | 主页：射频参数 + 加密密钥 + 固件升级 + 恢复出厂 + 维护模式 五块 |
| `/set` | GET | 读 freq/sf/bw/cr/power 写 NVS（经 `nvs_cfg_value_ok` 集中校验，有非法值整表拒写，P0-4） |
| `/crypto` | GET | `?key=<32hex>` 设密钥；`?off=1` 关加密 |
| `/factory` | POST | 恢复出厂默认（`nvs_cfg_factory` + 保存），页面按钮触发（P0-4） |
| `/update` | POST | multipart 上传 .bin；完成回调 `handle_update_done` 兜底失败 |
| `/exit` | GET | 退出维护模式（`ota_stop`） |

六个路由对应页面上的五个功能区加一个退出。


### 页面资源（webui.h）

独立成文件的原因：整页 HTML+CSS 约 9KB，塞在 `ota.cpp` 里会把业务逻辑冲掉；独立后改版只动一个文件。

11 个 `static const char ... PROGMEM` 段，由 `ota.cpp` 按固定顺序拼装（动态值插在段与段之间）：

| 段 | 内容 |
|---|---|
| `WUI_HEAD` | `<!DOCTYPE>` + 全部 CSS + 几何标记 SVG + 大标题（`<header>` 留给调用方闭合） |
| `WUI_SUB_OPEN` / `WUI_SUB_END` | 状态行（节点/角色/加密[/撞号冲突]）的壳，中间插动态值 |
| `WUI_RADIO_A` / `WUI_RADIO_B` | 射频参数区的壳，中间插 5 个动态字段 |
| `WUI_CRYPTO` | 加密密钥区（整段静态） |
| `WUI_OTA` | 固件升级区 |
| `WUI_FACTORY` | 恢复出厂区（红色危险按钮，P0-4 新增） |
| `WUI_TAIL` | 维护模式 + 页脚 + 收尾 JS |
| `WUI_DONE_A` / `WUI_DONE_B` | 回执页的壳，中间插一句结果提示 |

三条硬约束（改页面时必须守住）：

1. 板子自己开热点、手机没外网 → 不许引用任何外部资源，CSS/JS 全内嵌
2. Flash 每 KB 都要省（当前 55.7%）→ 不引框架，纯手写 HTML + 少量原生 JS
3. WebServer 单线程串行处理 → 页面越简单越稳

回执页复用主页页头：`send_done()` 用 `WUI_HEAD + 状态行 + WUI_DONE_A/B`，只多一套 22+179 字节的段，不重复一份 CSS。

校验标记永不回显：密钥输入框只给占位符，`value` 永远是空的 —— 回显密钥等于把密钥发给任何连上热点的人。


### 网页输入校验（前端 + 后端双重）

前端（`WUI_TAIL` 里的原生 JS）：

- 提交时把输入里的空白字符去掉，再判 `/^[0-9a-fA-F]{32}$/`
- 不通过 → `preventDefault()` 拦下提交，在输入框下方显示「请输入正确的格式」，不改动原密钥
- 通过 → 把清干净的值写回输入框再提交（避免「前端判过了、后端收到带空格的原文」这种前后端判定不一致）
- 「关闭加密」按钮走独立分支（`skip` 标记），不受该校验拦截

后端（`handle_crypto`）：同样先去掉空白，再交给 `crypto_set_key_hex()`；失败同样回「请输入正确的格式」。判定标准与 `crypto.cpp` 完全一致：恰好 32 个 hex 字符（`strlen(hex) != CRYPTO_KEY_LEN * 2` 即拒）。手机浏览器关掉 JS 也拦得住。

射频参数（`handle_set`）改为走 `nvs_cfg` 的集中校验（P0-4）：`freq 862–930`（按 `radios/*.h` 硬件能力宏）、`sf 7–11`、`bw ∈ {125,250,500}`、`cr 5–8`、`power -9–22` —— 与 CLI `set`、远程 `cfg` 完全同一套规则。有一个非法值就整表拒绝，不留半套参数进 NVS。

前后端各判一次，判据必须一致，否则会出现「前端放行、后端拒绝」或反之。


### 刷固件（标准 Update.h 流程）

- `UPLOAD_FILE_START` → `g_updating=true`（超时判定让路）→ `Update.begin(UPDATE_SIZE_UNKNOWN)`
- `UPLOAD_FILE_WRITE` → `Update.write(buf, size)`（分块写 flash）
- `UPLOAD_FILE_END` → `g_updating=false` → `Update.end(true)` → 回执 → 300ms → `ESP.restart()`
- 完成回调 `handle_update_done`：失败或没选文件时回一句「刷写失败」，避免旧实现那种浏览器一直转圈等不到回应
- 靠 otadata 自动切新分区；新固件启动后由 `ota_boot_confirm_tick()` 确认，否则下次重启回滚（P1-5）

三个回调各管一段：开始置标志、写入落 flash、结束回执并重启。


### 维护模式进出

- 进（`ota_start`）：`radio_suspend()` → `WiFi.mode(WIFI_AP)` → `softAP(ssid[, pass])` → `softAPConfig(192.168.4.1)` → `WebServer` 起路由 → 起 ota_task；同时记录 `g_ap_start_ms = g_last_client_ms = millis()`、`g_updating=false`
- 出（`ota_stop`）：只置 `g_active=false`，不直接删任务/服务；ota_task 循环检测到后自行清理并 `vTaskDelete(nullptr)` 删自己。原因：`handle_exit` 在 ota_task 上下文里跑，直接 `vTaskDelete(g_ota_task)` 会删自己导致后续 `radio_resume()` 永不执行 + 内存泄漏
- 自动退出（P1-5）：ota_task 每轮判 `softAPgetStationNum()`，有客户端就刷新 `g_last_client_ms`；`!g_updating` 时若「空闲 > 5 分钟」或「开启 > 30 分钟」→ `ota_stop()`（任务内部调用只置标志，本任务照常走自清理路径）

进出的不对称（进是同步建、出是置标志等自清理）是为了避开「自己删自己」的坑。


### 上电自动进入维护模式（S5，2026-10-17，条件式）

需求（用户定，学 ELRS）：没有电脑/USB 时也能靠 WiFi 进门 —— 上电后自动进维护模式，秒数可配（`auto_wifi_s`，默认 30，钳制 15–60，不能关闭）。

- 实现：`ota_auto_tick()`（`main.cpp` 的 `loop()` 每轮调）
- 判据（条件式）：上电满 `auto_wifi_s` 秒 且本次开机空口无任何活动 —— 四个现成信号全为空才算「无活动」：
  1. `mesh_rx_count()`（收到过有效帧）
  2. `mesh_drop_crc_count()`（收到过 CRC 错帧 → 空口确实有人在发）
  3. `route_neighbors()`（邻居表非空）
  4. `mav_router_heartbeat_ok()`（有 MAVLink 心跳 → 机载时飞控连着，不该开 AP）
- 只做一次：处理过（进或取消）就置 `g_auto_done`，用户主动 `wifi off` 退出后不会被再拉进去
- 取延时不看 `nvs_cfg_load()` 的返回值（那个返回值只表示 `role` 是否已设；未配置角色恰恰最需要自动进维护模式 —— S5b 修过这一处）
- 上板实测（无 E220）：`[ota] 上电 30s 无任何链路活动 → 自动进入 WiFi 维护模式` → `[ota] AP EMlink-187 @ 192.168.4.1`

四个信号只要有一个非空就不进 AP，是为了避免在正常组网时把链路打断。


### OTA 回滚确认（P1-5）

`ota_boot_confirm_tick()`：`g_boot_confirmed` 保护只做一次；`millis() < 10000` 直接返回；`!radio_is_ready()` 直接返回（不确认）；否则调 `esp_ota_mark_app_valid_cancel_rollback()` 并按返回值打日志（`ESP_OK` / `ESP_ERR_NOT_SUPPORTED` 平台未启用回滚 / 其它错误码）。
调用点：`main.cpp` 的 `loop()` 里每轮一次（`loopTask` 未订阅看门狗，10ms 一轮，开销可忽略）。


### 关键实现细节

1. 整页用 `String` 拼装后一次 `send()`：`String::reserve(WUI_RESERVE=10240)` 一次分配、避免堆碎片；Content-Length 和 `Connection: close` 交给 WebServer 填。
   旧实现是「`sendHeader()` 设 Content-Type + 手写 `HTTP/1.1 200 OK` 状态行 + 逐字节 `pgm_read_byte` 写 body」，有两个毛病：① `sendHeader()` 只是往 `_responseHeaders` 里追加，而只有 `send()`/`_prepareHeader()` 才会把它发出去并清空 —— 旧代码从不调 `send()`，于是每来一个请求这串就长一截，永不释放（String 堆泄漏）；② Content-Type 其实从没真正发给浏览器，页面能正常显示纯属浏览器嗅探。改法见 `ota.cpp` 顶部注释。
2. 射频 suspend 后 no-op：`radio_transmit/readPacket/startReceive/set_sf/channelActivity` 开头都查 `g_suspended`，暂停期上层模块照常跑但都不碰 SPI/芯片。P0-2 起这些接口都经射频单所有者 `rf_task` 执行 —— `radio_suspend/resume` 本身也是请求，会阻塞到所有者真正执行完，保证「WiFi 起来前射频确实已停」。
3. nvs_cfg 新增字段：`wifi_ssid[32]`/`wifi_pass[32]`/`ota_url[96]`；SSID 空则默认 `EMlink-<node_id>`，pass 空则开放热点，ota_url 预留。
4. 不接板子验证页面：`design/verify_webui.py` 会把 `webui.h` 里 11 个段的 C 字符串抠出来（按「是否在字符串内」扫描，不能在 `;` 上简单截断 —— CSS 里到处是分号），按 `ota.cpp` 的拼装顺序还原成真实 HTML，做标签闭合/关键元素/页面尺寸自检，并输出 `design/webui-render.html` 供浏览器直接打开。
   验证链路：`python3 design/verify_webui.py`（页面对不对）→ `pio run`（能不能编译）→ Firefox headless 截图看观感。只做 `pio run` 只能证明「能编译」，证明不了「页面是对的」。
   本机 Microsoft Edge 的 headless 稳定 SIGTRAP 崩溃（换 4 组参数都一样），截图请用 `firefox --headless --screenshot`；另注意跑浏览器前要 `unset LD_LIBRARY_PATH`（Hermes 运行时会把旧 libstdc++ 塞进去）。
5. 无鉴权（已知，未修）：连上 AP 的人可以改参数、看/设密钥、刷固件。本次只做了「超时自动关 + 回滚确认」；鉴权策略（强制密码 / 一次性 PIN / 仅 USB 已枚举时可用）属产品决策，待定。

第 1 条是修过的堆泄漏，第 4 条是无硬件验证页面的方法，第 5 条是已知的产品级缺口。
