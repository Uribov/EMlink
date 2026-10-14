# ota —— OTA 固件升级 / WiFi Web 管理

## 职责
落地维护时的无线管理入口：板子开一个 WiFi 热点，手机连上后打开网页，就能**改参数、设加密密钥、刷固件**，不用插 USB 线。

学 ELRS 的做法：WiFi AP 模式 + 内嵌网页 + 浏览器上传 .bin 刷固件。网页视觉也学 ELRS（深色单页、大标题、扁平几何），但**不抄它的代码**——ELRS 的实现是 Vue SPA（压缩后 250–320KB），本工程既放不下也不需要。

## 状态
**已实现**。默认不启动；CLI 敲 `wifi` 触发。

## 依赖
- `WiFi.h` / `WebServer.h` / `Update.h`（ESP32 Arduino 内置，无需额外库）
- `nvs_cfg.h`（读写参数/密钥/node_id/wifi 配置）、`crypto.h`（设密钥）
- `radio.h`（`radio_suspend`/`radio_resume`，维护模式二选一）
- `webui.h`（页面静态资源：HTML 骨架 + CSS + 几何标记，仅本模块用）

## 方案（定死）
| 项 | 值 |
|---|---|
| 模式 | WiFi AP（自己当热点，不连外网） |
| SSID | `EMlink-<node_id>`（可用 `wifi_ssid` 覆盖） |
| IP | 192.168.4.1 |
| 密码 | 默认无密码（`wifi_pass` 可设） |
| 网页功能 | 改参数 / 设密钥 / 刷固件(Update.h) / 退出维护模式 |
| 网页风格 | 学 ELRS：深色底、大标题、纯黑白灰、1px 发丝线、零圆角、等边三角网格标记 |
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
- **网页密钥填不足 32 位（或含非 hex 字符）→ 报「请输入正确的格式」，不提交、不改动原密钥**
- 网页密钥里粘贴时带了空格/换行 → 自动清掉后仍判定通过
- 网页上传 .bin → 刷写 → 自动重启 → 版本更新
- 网页上传时**没选文件** → 立刻回「刷写失败」，不转圈
- 网页「退出」→ LoRa 恢复，`stats` 显示链路正常

---

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_server`：`WebServer*`（端口 80），**由 ota_task 唯一持有/删除**
- `g_ota_task`：ota_task 句柄（core1，优先级 6，栈 8KB）
- `g_active`：维护模式标志（`volatile`，ota_stop 只置 false）
- `g_node_id`：SSID 用的节点号（ota_start 时从 NVS 读）
- `g_upd_ok`：本次刷写是否成功；`/update` 的**完成回调**靠它区分「成功（已回过话并重启）」和「失败/压根没选文件」

### 任务
- `ota_task`（core1 prio6）：`while (g_active)` 循环 `g_server->handleClient()`；g_active 变 false 后**自行清理**（停/删 g_server、关 WiFi、radio_resume）再 `vTaskDelete(nullptr)` 删自己
- 跑 core1 而非 core0：WiFi 栈占 core0，射频也 core0，WebServer 放 core1 不抢

### 路由表（WebServer）
| 路径 | 方法 | 作用 |
|---|---|---|
| `/` | GET | 主页：射频参数 + 加密密钥 + 固件升级 + 维护模式 四块 |
| `/set` | GET | 读 freq/sf/bw/cr/power 写 NVS（**先白名单校验，有非法值整表拒写**） |
| `/crypto` | GET | `?key=<32hex>` 设密钥；`?off=1` 关加密 |
| `/update` | POST | multipart 上传 .bin；完成回调 `handle_update_done` 兜底失败 |
| `/exit` | GET | 退出维护模式（`ota_stop`） |

### 页面资源（webui.h）
独立成文件的原因：整页 HTML+CSS 约 9KB，塞在 `ota.cpp` 里会把业务逻辑冲掉；独立后改版只动一个文件。

**10 个 `static const char ... PROGMEM` 段**，由 `ota.cpp` 按固定顺序拼装（动态值插在段与段之间）：

| 段 | 内容 |
|---|---|
| `WUI_HEAD` | `<!DOCTYPE>` + 全部 CSS + 几何标记 SVG + 大标题（`<header>` 留给调用方闭合） |
| `WUI_SUB_OPEN` / `WUI_SUB_END` | 状态行（节点/角色/加密）的壳，中间插动态值 |
| `WUI_RADIO_A` / `WUI_RADIO_B` | 射频参数区的壳，中间插 5 个动态字段 |
| `WUI_CRYPTO` | 加密密钥区（整段静态） |
| `WUI_OTA` | 固件升级区 |
| `WUI_TAIL` | 维护模式 + 页脚 + 收尾 JS |
| `WUI_DONE_A` / `WUI_DONE_B` | 回执页的壳，中间插一句结果提示 |

**三条硬约束**（改页面时必须守住）：
1. 板子自己开热点、手机没外网 → **不许引用任何外部资源**，CSS/JS 全内嵌
2. Flash 每 KB 都要省（当前 55.2%）→ 不引框架，纯手写 HTML + 少量原生 JS
3. WebServer 单线程串行处理 → 页面越简单越稳

**回执页复用主页页头**：`send_done()` 用 `WUI_HEAD + 状态行 + WUI_DONE_A/B`，只多一套 22+179 字节的段，不重复一份 CSS。

**校验标记永不回显**：密钥输入框只给占位符，`value` 永远是空的——回显密钥等于把密钥发给任何连上热点的人。

### 网页输入校验（前端 + 后端双重）
前端（`WUI_TAIL` 里的原生 JS）：
- 提交时把输入里的空白字符去掉，再判 `/^[0-9a-fA-F]{32}$/`
- 不通过 → `preventDefault()` 拦下提交，在输入框下方显示**「请输入正确的格式」**，不改动原密钥
- 通过 → 把清干净的值写回输入框再提交（避免「前端判过了、后端收到带空格的原文」这种前后端判定不一致）
- 「关闭加密」按钮走独立分支（`skip` 标记），不受该校验拦截

后端（`handle_crypto`）：同样先去掉空白，再交给 `crypto_set_key_hex()`；失败同样回「请输入正确的格式」。判定标准与 `crypto.cpp` **完全一致：恰好 32 个 hex 字符**（`strlen(hex) != CRYPTO_KEY_LEN * 2` 即拒）。手机浏览器关掉 JS 也拦得住。

射频参数（`handle_set`）同样白名单校验：`freq 400–1000`、`sf 7–11`、`bw ∈ {125,250,500}`、`cr 5–8`、`power -9–30`。原本只靠 `<select>` 限制，构造 URL 就能塞进 `freq=0` 把射频写死；现在**有一个非法值就整表拒绝**，不留半套参数进 NVS。

### 刷固件（标准 Update.h 流程）
- `UPLOAD_FILE_START` → `Update.begin(UPDATE_SIZE_UNKNOWN)`
- `UPLOAD_FILE_WRITE` → `Update.write(buf, size)`（分块写 flash）
- `UPLOAD_FILE_END` → `Update.end(true)` → 回执 → 300ms → `ESP.restart()`
- 完成回调 `handle_update_done`：失败或**没选文件**时回一句「刷写失败」，避免旧实现那种浏览器一直转圈等不到回应
- 靠 otadata 自动切新分区；写坏可回滚

### 维护模式进出
- 进（`ota_start`）：`radio_suspend()` → `WiFi.mode(WIFI_AP)` → `softAP(ssid[, pass])` → `softAPConfig(192.168.4.1)` → `WebServer` 起路由 → 起 ota_task
- 出（`ota_stop`）：**只置 `g_active=false`**，不直接删任务/服务；ota_task 循环检测到后自行清理并 `vTaskDelete(nullptr)` 删自己。原因：`handle_exit` 在 ota_task 上下文里跑，直接 `vTaskDelete(g_ota_task)` 会删自己导致后续 `radio_resume()` 永不执行 + 内存泄漏

### 关键实现细节
1. **整页用 `String` 拼装后一次 `send()`**：`String::reserve(WUI_RESERVE=10240)` 一次分配、避免堆碎片；Content-Length 和 `Connection: close` 交给 WebServer 填。
   旧实现是「`sendHeader()` 设 Content-Type + 手写 `HTTP/1.1 200 OK` 状态行 + 逐字节 `pgm_read_byte` 写 body」，有两个毛病：① `sendHeader()` 只是往 `_responseHeaders` 里追加，而只有 `send()`/`_prepareHeader()` 才会把它发出去并清空——旧代码从不调 `send()`，于是**每来一个请求这串就长一截，永不释放**（String 堆泄漏）；② Content-Type 其实从没真正发给浏览器，页面能正常显示纯属浏览器嗅探。改法见 `ota.cpp` 顶部注释。
2. **射频 suspend 后 no-op**：`radio_transmit/readPacket/startReceive/set_sf/channelActivity` 开头都查 `g_suspended`，暂停期上层模块照常跑但都不碰 SPI/芯片。
3. **nvs_cfg 新增字段**：`wifi_ssid[32]`/`wifi_pass[32]`/`ota_url[96]`；SSID 空则默认 `EMlink-<node_id>`，pass 空则开放热点，ota_url 预留。
4. **不接板子验证页面**：`design/verify_webui.py` 会把 `webui.h` 里 10 个段的 C 字符串抠出来（按「是否在字符串内」扫描，不能在 `;` 上简单截断——CSS 里到处是分号），按 `ota.cpp` 的拼装顺序还原成真实 HTML，做标签闭合/关键元素/页面尺寸自检，并输出 `design/webui-render.html` 供浏览器直接打开。
   验证链路：`python3 design/verify_webui.py`（页面对不对）→ `pio run`（能不能编译）→ Firefox headless 截图看观感。只做 `pio run` 只能证明「能编译」，证明不了「页面是对的」。
   ⚠️ 本机 Microsoft Edge 的 headless 稳定 SIGTRAP 崩溃（换 4 组参数都一样），截图请用 `firefox --headless --screenshot`；另注意跑浏览器前要 `unset LD_LIBRARY_PATH`（Hermes 运行时会把旧 libstdc++ 塞进去）。
