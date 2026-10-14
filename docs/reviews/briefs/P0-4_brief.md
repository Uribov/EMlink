# EMlink M0 修复 2/4：配置集中校验 + node_id 自动分配 + 恢复出厂 + 射频初始化失败可视化（P0-4）

## 背景
审查发现四件事（P0-4）：
1. `nvs_cfg_set()` 对值和键**零校验**：CLI `set freq 0` / 远程 `cfg sf 99` 都能写进 NVS，重启后射频起不来；
2. 网页校验范围与芯片不符（放行 power=30，而 SX1268 上限 22 → `begin()` 失败）；
3. `node_id` 默认全 1（NVS 0=自动分配但没实现）→ 两台默认配置的节点撞号，去重/路由互相污染，且**没有任何撞号检测**；
4. 射频 `radio_init` 失败后 `main.cpp` 重试一次就静默继续 → 得到一台"没有射频、没有告警"的节点。

本次修复目标（P0 级，不碰其他 P1/P2 项）：
- **A. 校验集中**：唯一入口 `nvs_cfg_value_ok(key,val)`，CLI / 远程 FRAME_CFG / 网页共用；
- **B. node_id**：首次上电从芯片 MAC 派生 1..254 并落盘；收到"自称本机号"的陌生帧 → 撞号计数，CLI `status` 与网页状态行可见；
- **C. 恢复出厂**：CLI `factory` 命令 + 网页"恢复出厂"按钮（POST `/factory`）；
- **D. 射频初始化失败**：`radio_is_ready()` 状态位 + `rf_task` 每 10s 自动重试 + LED 整灯红 + CLI `status` 显示。

## 只允许修改这些文件（10 个）
1. `firmware/src/radios/e220-900m22s.h`（加硬件能力宏）
2. `firmware/src/service/nvs_cfg.h` / `.cpp`
3. `firmware/src/service/ota.cpp`
4. `firmware/src/service/webui.h`
5. `firmware/src/app/cli.cpp`
6. `firmware/src/drivers/radio.h` / `.cpp`
7. `firmware/src/net/mesh.h` / `.cpp`
8. `firmware/src/app/stats.cpp`
9. `firmware/src/main.cpp`
10. `design/verify_webui.py`

---

## A. 硬件能力宏（`radios/e220-900m22s.h`）
在文件末尾追加（注释说明：改射频模块时一起改这里）：
```c
// ---- 硬件能力（nvs_cfg 集中校验用；换射频模块时同步改）----
#define RADIO_FREQ_MIN_MHZ   862.0f   // E220-900M22S 频段下限
#define RADIO_FREQ_MAX_MHZ   930.0f   // 频段上限
#define RADIO_PWR_MIN_DBM    (-9)     // SX1268 输出功率范围
#define RADIO_PWR_MAX_DBM    22
```

## B. `nvs_cfg.h` / `nvs_cfg.cpp`
### B1. 新增两个公开接口（`nvs_cfg.h` 声明）
```c
// 取值范围校验：CLI / 远程 FRAME_CFG / 网页共用同一套（nvs_cfg_set 内部先调它）
bool nvs_cfg_value_ok(const char* key, const char* val);

// 从芯片 MAC 派生 1..254 的节点号（首次上电用；同板稳定，不同板大概率不同）
uint8_t nvs_cfg_assign_node_id(void);
```
### B2. 校验规则表（`nvs_cfg.cpp` 实现，用范围/白名单，全部拒绝非数字垃圾）
在 nvs_cfg.cpp 顶部 `#include "../pinmap.h"`（拿到上面的硬件能力宏）。
| key | 规则 |
|---|---|
| `role` | 仅 `air` / `gcs` / `relay` |
| `node_id` | 整数 1..254 |
| `sysid` | 整数 0..255 |
| `freq` | 浮点 `RADIO_FREQ_MIN_MHZ..RADIO_FREQ_MAX_MHZ` |
| `sf` | 整数 7..11 |
| `bw` | 仅 125 / 250 / 500 |
| `cr` | 整数 5..8 |
| `power` | 整数 `RADIO_PWR_MIN_DBM..RADIO_PWR_MAX_DBM`（即 -9..22） |
| `syncword` | 整数 0..65535 |
| `uart_baud` | 白名单 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600 |
| `ttl` | 整数 1..10 |
| `ack_timeout_ms` | 整数 50..10000 |
| `retries` | 整数 0..10 |
| `beacon_s` | 整数 1..60 |
| `failsafe_s` | 整数 1..60 |
| `name` | 长度 1..15 |
| 其它未知键 | 一律 false |

实现要求：
- 字符串→数字用 `strtol/strtod` 并**检查尾部**（`"12abc"` 必须判非法；`strtol` 的 endptr 必须指向字符串结尾）；
- `nvs_cfg_set()` 开头加 `if (!nvs_cfg_value_ok(key, val)) return false;`（原写入逻辑不动）；
- `nvs_cfg_apply_kv()`（远程 FRAME_CFG）保持"白名单拒 role"，其余键自动经 `nvs_cfg_set` 获得校验——不需要另加代码。

### B3. `nvs_cfg_assign_node_id()`
```c
uint8_t mac[6] = {0};
esp_read_mac(mac, ESP_MAC_WIFI_STA);      // #include <esp_mac.h>
return (uint8_t)(1 + ((mac[3] ^ mac[4] ^ mac[5]) % 254));   // 1..254
```

## C. `main.cpp` —— 首次上电落盘 node_id + 射频失败不静默
在 `nvs_cfg_load(&g_cfg);` 之后 `crypto_init();` 之前插入：
```c
    // 首次上电（NVS 里 node_id=0）：从芯片 MAC 派生并落盘，避免全网默认 1 号撞号
    if (g_cfg.node_id == 0) {
        g_cfg.node_id = nvs_cfg_assign_node_id();
        nvs_cfg_save(&g_cfg);
        Serial.printf("[cfg] node_id 自动分配 = %u（MAC 派生，已存 NVS）\n", g_cfg.node_id);
    }
```
把原来"初始化失败就 50ms 后再试一次、仍失败也静默继续"的写法改成**单次尝试 + 明确日志**（周期重试由 rf_task 负责，见 E）：
```c
    if (!radio_init(g_cfg.freq, g_cfg.bw, g_cfg.sf, g_cfg.cr, g_cfg.syncword, g_cfg.power)) {
        Serial.println("[radio] 初始化失败：LED 红灯，CLI status 可见；rf_task 每 10s 自动重试");
    }
```

## D. `mesh.cpp` / `mesh.h` —— 撞号检测
### D1. 发送序号历史环（mesh.cpp）
- 新增 static：`#define MESH_SEQ_HISTORY 32`；`static uint16_t g_my_seq_hist[MESH_SEQ_HISTORY];`；`static uint8_t g_my_seq_idx;`；`static uint32_t g_id_conflict;`
- `mesh_init()` 里把历史环填 `0xFFFF`、索引归零、`g_id_conflict = 0`；
- `mesh_next_seq()`：在现有 portMUX 临界区内，把分配的 seq 写入历史环并推进索引（**保持唯一 seq 源不变**）；
- 新增 `static bool seq_is_mine(uint16_t s)`：在 portMUX 内查历史环。
### D2. mesh_task_func 里检测（`frame_decode` 成功之后、ACK 分支之前）
```c
        // 撞号检测：收到“自称本机 node_id”的帧，但 seq 不在本机最近 32 个已发序号里
        // → 极有可能是另一台节点用了同一个 node_id（本机自己中继回来的帧会在历史里，不计）
        if (f.src_id == g_my_node_id && !seq_is_mine(f.seq)) {
            g_id_conflict++;
        }
```
### D3. 对外接口（`mesh.h` 声明 + mesh.cpp 实现）
```c
uint32_t mesh_id_conflict_count(void);
```

## E. `radio.h` / `radio.cpp` —— 就绪状态 + 每 10s 自动重试
### E1. radio.cpp static 新增
- `static volatile bool g_radio_ready = false;`
- 记录初始化参数：`static float g_init_freq, g_init_bw; static uint8_t g_init_sf, g_init_cr; static uint16_t g_init_sync; static int8_t g_init_pwr;`
### E2. `radio_init()`：进来先存参数；成功置 `g_radio_ready = true`，失败置 `false`（返回值/自救逻辑不动）
### E3. 新增接口（radio.h 声明、radio.cpp 实现）
```c
// 射频是否就绪（init 成功且未被撤下）。未就绪时所有射频操作直接失败返回，不碰 SPI
bool radio_is_ready(void);
```
### E4. 所有射频操作加"未就绪"短路（`radio_transmit` / `radio_startReceive` / `radio_readPacket` / `radio_channelActivity` / `radio_set_sf`：在原有 `g_suspended || !g_radio` 判断后**加上** `|| !g_radio_ready`，用同一个短路条件）
### E5. `rf_task` 每 10s 重试（新增 static 函数，循环里调用）
```c
#define RADIO_RETRY_MS 10000

// 射频初始化失败后的周期重试（不触发 watchdog_rf_fault，避免"累计 3 次整机重启"被重试放大）
static void radio_retry_tick() {
    if (g_radio_ready || !g_radio || g_suspended) return;
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < RADIO_RETRY_MS) return;
    last_ms = now;

    int16_t st = g_radio->begin(g_init_freq, g_init_bw, g_init_sf, g_init_cr, g_init_sync, g_init_pwr, 8, 0.0f);
    if (st == RADIOLIB_ERR_NONE) {
        attachInterrupt(digitalPinToInterrupt(PIN_LORA_DIO1), radio_isr, RISING);
        g_radio_ready = true;
        Serial.println("[radio] 初始化重试成功，射频已恢复");
    } else {
        Serial.printf("[radio] 初始化重试失败（%d），%dms 后再试\n", (int)st, RADIO_RETRY_MS);
    }
}
```
在 `rf_task_func` 的循环体里**每轮**调用 `radio_retry_tick();`（位置：喂狗之后、等通知之前；射频未就绪时不会有真包，逻辑安全）。
注意：首次 `radio_init` 里成功的 `attachInterrupt` 保持不变。

## F. `stats.cpp` —— 射频未就绪 → LED 整灯红
在 `stats_task_func` 的 LED 更新处：
```c
        led_health_t lh = failsafe_link_lost() ? LED_HEALTH_LOST : LED_HEALTH_OK;
        if (!radio_is_ready()) lh = LED_HEALTH_FAULT;   // 射频未就绪：整灯红（覆盖角色色）
        stats_update_led(lr, lh);
```
（`stats.cpp` 已 include radio.h；`LED_HEALTH_FAULT` 已有红覆盖逻辑，不要改 `stats_update_led` 本身。）

## G. `cli.cpp`
1. `help` 里加一行：`  factory              恢复出厂默认（清 role/密钥/参数）\r\n`
2. 新增命令（放在 `reset` 命令附近）：
```c
    } else if (strcmp(cmd, "factory") == 0) {
        cfg_t c;
        nvs_cfg_factory(&c);
        if (nvs_cfg_save(&c)) cli_print("已恢复出厂默认（role 已清空、密钥已清除）——重启后进入配置模式\r\n");
        else                  cli_print("恢复失败：NVS 写入错误\r\n");
    }
```
3. `status` 命令扩充为三行（保持原有 role 行不动，追加两行）：
```c
    } else if (strcmp(cmd, "status") == 0) {
        char val[64];
        char line[128];
        nvs_cfg_get("role", val, sizeof(val));
        snprintf(line, sizeof(line), "role=%s\r\n", val);
        cli_print(line);
        nvs_cfg_get("node_id", val, sizeof(val));
        snprintf(line, sizeof(line), "node_id=%s  撞号冲突=%u\r\n", val, (unsigned)mesh_id_conflict_count());
        cli_print(line);
        snprintf(line, sizeof(line), "RF: %s\r\n",
                 radio_is_ready() ? "就绪" : "初始化失败（每 10s 重试，LED 红灯）");
        cli_print(line);
    }
```
（cli.cpp 已 include radio.h 与 mesh.h，确认无需新增 include；若缺就补。）
4. `set` 失败提示改为：`"设置失败：键名未知或值超出范围（用 get 查看当前值）\r\n"`。

## H. `ota.cpp` + `webui.h` —— 网页复用校验 + 恢复出厂按钮 + 状态行撞号提示
### H1. `handle_set()` 改为调用集中校验（替换原来的 if 范围判断）
```c
    bool bad = false;
    if (s.hasArg("freq"))  { const char* v = s.arg("freq").c_str();  if (nvs_cfg_value_ok("freq", v))  c.freq  = (float)atof(v); else bad = true; }
    if (s.hasArg("sf"))    { const char* v = s.arg("sf").c_str();    if (nvs_cfg_value_ok("sf", v))    c.sf    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("bw"))    { const char* v = s.arg("bw").c_str();    if (nvs_cfg_value_ok("bw", v))    c.bw    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("cr"))    { const char* v = s.arg("cr").c_str();    if (nvs_cfg_value_ok("cr", v))    c.cr    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("power")) { const char* v = s.arg("power").c_str(); if (nvs_cfg_value_ok("power", v)) c.power = (int8_t)atoi(v); else bad = true; }
    if (bad) { send_done(s, true, "请输入正确的格式"); return; }
```
### H2. 新增恢复出厂路由
```c
// ---- 恢复出厂 /factory ----（提示后由用户在页面点“返回”）
static void handle_factory() {
    cfg_t c;
    nvs_cfg_factory(&c);
    if (!nvs_cfg_save(&c)) { send_done(*g_server, true, "写入失败：NVS 保存出错。"); return; }
    send_done(*g_server, false, "已恢复出厂默认（role 已清空、密钥已清除）。请断电重启后重新配置。");
}
```
注册：`g_server->on("/factory", HTTP_POST, handle_factory);`
### H3. `add_status()` 增加撞号提示（有冲突才显示）
在现有三个 span 之后追加：
```c
    uint32_t cf = mesh_id_conflict_count();
    if (cf > 0) {
        snprintf(b, sizeof(b), "<span class=\"st bad\"><i></i>同号冲突 %u</span>", (unsigned)cf);
        p += b;
    }
```
（ota.cpp 顶部补 `#include \"../net/mesh.h\"`。）
### H4. `webui.h`
- 段 1 的 CSS 里，紧跟 `.st.off i{background:var(--dim)}` 之后加一条：`.st.bad i{background:var(--err)}`；
- 新增一段（放在 WUI_OTA 段之后、WUI_TAIL 段之前）：
```c
// ---- 段 6：恢复出厂区（危险操作，放页尾区）----
static const char WUI_FACTORY[] PROGMEM =
"<section><h2>恢复出厂</h2>"
"<form action=\"/factory\" method=\"post\">"
"<button class=\"btn danger\" type=\"submit\">恢复出厂默认</button></form>"
"<div class=\"note\">清空 role / 密钥 / 全部参数，重启后需重新配置</div></section>";
```
- `ota.cpp` 的 `handle_root()` 拼装顺序改为：`... WUI_OTA + WUI_FACTORY + WUI_TAIL`
### H5. `design/verify_webui.py`
- 主页拼装加上 `S['WUI_FACTORY']`（顺序与 handle_root 一致）；
- `must` 列表补：`'action="/factory"'`、`'恢复出厂默认'`、`'class="st bad"'` 之外的——只补前两个即可（第三个是动态生成的，不在静态页里）。

## 硬约束
- 只改上面列的文件；**不要**动 `route.*` / `frag.*` / `addr_map.*` / `crypto.*` / `watchdog.*` / `failsafe_*` / `uart_fc.*` / `usb_cdc.*` / `power_detect.*` / `rtos.*` 与任何 `.md`；
- 不改现有帧格式、不动 QoS/ACK/去重/路由逻辑（那是后续 M0 项）；
- 校验规则表必须**严格按上表**（不要自行放宽/收紧）；
- 中文注释，风格与现有代码一致；注释里简短说明"为什么"；
- **不要运行编译命令**、**不要 git commit**、不要跑 verify_webui.py（验证由调用方做）；
- 保持最小 diff。

## 完成后请输出
1. `git diff --stat`；
2. 5~8 行中文总结：每个文件改了什么。
