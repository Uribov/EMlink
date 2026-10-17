// ============================================================
// ota.cpp —— OTA 固件升级 / WiFi Web 管理
// WiFi AP + WebServer + 内嵌 HTML（页面资源见 webui.h，学 ELRS 视觉）。
// 改参数 / 设密钥 / 刷固件(Update.h)；进维护模式 radio_suspend()，退出恢复。
// 页面用 String 拼装后一次 send()：Content-Length 由框架填、连接头由框架清，
// 避免旧实现「手动写状态行 + sendHeader」导致 _responseHeaders 只增不清的泄漏。
// ============================================================
#include "ota.h"
#include "webui.h"
#include "../rtos.h"
#include "nvs_cfg.h"
#include "crypto.h"
#include "../drivers/radio.h"
#include "../service/role_state.h"
#include "../net/mesh.h"
#include "../net/route.h"
#include "../service/mav_router.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <stdio.h>
#include <stdlib.h>

static WebServer*   g_server = nullptr;
static TaskHandle_t g_ota_task = nullptr;
static volatile bool g_active = false;
static uint8_t      g_node_id = 1;
static bool         g_upd_ok  = false;   // 本次刷写是否成功（供完成回调判断）

// 维护模式自动退出（P1-5）：AP 是无鉴权的改参数/刷固件入口，不能长期敞开
#define OTA_AP_IDLE_MS       300000u   // 5 分钟没有客户端连着 → 自动退出
#define OTA_AP_MAX_MS       1800000u   // 30 分钟硬上限（刷固件过程中不生效）
#define OTA_BOOT_CONFIRM_MS   10000u   // 开机满 10s 且射频就绪才确认新固件

static uint32_t g_ap_start_ms = 0;      // AP 开启时刻
static uint32_t g_last_client_ms = 0;   // 最后一次"有客户端"的时刻
static bool     g_updating = false;     // 正在上传固件（期间不自动退出）
static bool     g_boot_confirmed = false;  // 本次开机是否已确认固件

// 整页 String 预留容量（选项化后主页约 12.4KB，留 ~4KB 余量；
// 一次分配，避免反复 realloc 造成堆碎片）
#define WUI_RESERVE 16384

// 字符串 → 角色号（网页提交用；nvs_cfg 的 role_from_str 是内部 static，不能直接调）
static uint8_t role_from_name(const char* s) {
    if (s && strcmp(s, "air") == 0)   return ROLE_AIR;
    if (s && strcmp(s, "gcs") == 0)   return ROLE_GCS;
    if (s && strcmp(s, "relay") == 0) return ROLE_RELAY;
    return ROLE_UNSET;
}

// 角色号 → 字符串（网页显示用；与 nvs_cfg 内部约定一致）
static const char* role_name(uint8_t r) {
    switch (r) {
        case ROLE_AIR:   return "air";
        case ROLE_GCS:   return "gcs";
        case ROLE_RELAY: return "relay";
        default:         return "unset";
    }
}

// ---- 状态行：节点 / 角色 / 加密 ----
static void add_status(String& p, const cfg_t& c) {
    char b[192];
    bool enc = crypto_enabled();
    snprintf(b, sizeof(b),
             "<span>节点 <b>%02u</b></span><span>角色 <b>%s</b></span>"
             "<span class=\"st%s\"><i></i>%s</span>",
             (unsigned)(c.node_id ? c.node_id : g_node_id),
             role_name(c.role),
             enc ? "" : " off",
             enc ? "加密已启用" : "加密未启用");
    p += b;
    // 撞号冲突提示（仅当检测到冲突才显示，静态页里看不到，运行时注入）
    uint32_t cf = mesh_id_conflict_count();
    if (cf > 0) {
        snprintf(b, sizeof(b), "<span class=\"st bad\"><i></i>同号冲突 %u</span>", (unsigned)cf);
        p += b;
    }
}

// ---- <option> 列表（cur 命中则加 selected）----
static void add_opts(String& p, const int* v, int n, int cur) {
    char b[32];
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "<option%s>%d</option>",
                 (v[i] == cur) ? " selected" : "", v[i]);
        p += b;
    }
}

// ---- 射频参数字段：值取自 NVS 当前配置（不再是写死的占位值）----
static void add_radio_fields(String& p, const cfg_t& c) {
    static const int SFV[] = {7, 8, 9, 10, 11};
    static const int BWV[] = {125, 250, 500};
    static const int CRV[] = {5, 6, 7, 8};
    static const int PWV[] = {10, 14, 17, 20, 22};   // E220 模块最大 22dBm；芯片范围 -9~22
    char b[288];

    // 频率：整数 + 小数(0.05 步进)，均为下拉（不再手输）
    int fi = (int)c.freq;
    int ff = (int)((c.freq - (float)fi) * 100.0f + 0.5f);   // 百分位
    if (ff % 5) ff -= ff % 5;                                 // 归到 0.05 步进
    if (ff > 95) ff = 95;

    p += "<div><label>频率配置</label><div class=\"row\">"
         "<select id=\"fi\" name=\"freq_int\">";
    for (int v = (int)RADIO_FREQ_MIN_MHZ; v <= (int)RADIO_FREQ_MAX_MHZ; v++) {
        snprintf(b, sizeof(b), "<option%s>%d</option>", (v == fi) ? " selected" : "", v);
        p += b;
    }
    p += "</select><select id=\"ff\" name=\"freq_frac\">";
    for (int i = 0; i < 20; i++) {
        snprintf(b, sizeof(b), "<option value=\"%.2f\"%s>%.2f</option>",
                 i * 0.05, (i * 5 == ff) ? " selected" : "", i * 0.05);
        p += b;
    }
    p += "</select></div></div>";

    // 扩频因子 / 带宽 / 编码率（下拉，保持原样）
    p += "<div><label for=\"sf\">扩频因子</label><select id=\"sf\" name=\"sf\">";
    add_opts(p, SFV, 5, c.sf);
    p += "</select></div><div><label for=\"bw\">带宽 kHz</label><select id=\"bw\" name=\"bw\">";
    add_opts(p, BWV, 3, c.bw);
    p += "</select></div><div><label for=\"cr\">编码率</label><select id=\"cr\" name=\"cr\">";
    add_opts(p, CRV, 4, c.cr);
    p += "</select></div>";

    // 发射功率：下拉
    p += "<div><label for=\"pw\">发射功率 dBm</label><select id=\"pw\" name=\"power\">";
    add_opts(p, PWV, 5, c.power);
    p += "</select></div>";

    // 节点号：滑块（实时显示）
    unsigned nid = (unsigned)(c.node_id ? c.node_id : g_node_id);
    snprintf(b, sizeof(b),
             "<div><label for=\"nid\">节点号 <span id=\"nidv\">%u</span></label>"
             "<input id=\"nid\" name=\"node_id\" type=\"range\" min=\"1\" max=\"254\" step=\"1\" value=\"%u\""
             " oninput=\"document.getElementById('nidv').textContent=this.value\"></div>",
             nid, nid);
    p += b;

    // 角色（下拉；空串 = 未配置）——保持原样
    {
        static const char* R_V[3] = {"air", "gcs", "relay"};
        static const char* R_L[3] = {"机载 AIR", "地面站 GCS", "中继 RELAY"};
        p += "<div><label for=\"rl\">角色</label><select id=\"rl\" name=\"role\">";
        p += (c.role == ROLE_UNSET) ? "<option value=\"\" selected>未配置</option>"
                                    : "<option value=\"\">未配置</option>";
        for (int i = 0; i < 3; i++) {
            snprintf(b, sizeof(b), "<option value=\"%s\"%s>%s</option>",
                     R_V[i], (c.role == (uint8_t)(i + 1)) ? " selected" : "", R_L[i]);
            p += b;
        }
        p += "</select></div>";
    }

    // 上电自动进维护模式：滑块（实时显示）
    snprintf(b, sizeof(b),
             "<div class=\"wide\"><label for=\"aws\">上电自动进维护模式 <span id=\"awsv\">%u</span> 秒（15–60）</label>"
             "<input id=\"aws\" name=\"auto_wifi_s\" type=\"range\" min=\"15\" max=\"60\" step=\"1\" value=\"%u\""
             " oninput=\"document.getElementById('awsv').textContent=this.value\"></div>",
             (unsigned)c.auto_wifi_s, (unsigned)c.auto_wifi_s);
    p += b;
}

// ---- 发送整页：Content-Length / Connection 均由 WebServer 负责 ----
static void send_page(WebServer& s, const String& page) {
    s.send(200, "text/html; charset=utf-8", page);
}

// ---- 回执页（复用主页页头 + 状态行，只有中间一句提示不同；省一套 CSS 的 Flash）----
static void send_done(WebServer& s, bool bad, const char* text) {
    cfg_t c;
    nvs_cfg_load(&c);

    String p;
    p.reserve(WUI_RESERVE);
    p += WUI_HEAD;
    p += WUI_SUB_OPEN;
    add_status(p, c);
    p += WUI_SUB_END;
    p += WUI_DONE_A;
    p += bad ? "<div class=\"msg bad\">" : "<div class=\"msg\">";
    p += text;
    p += "</div>";
    p += WUI_DONE_B;
    send_page(s, p);
}

// ---- 主页 ----
static void handle_root() {
    WebServer& s = *g_server;
    cfg_t c;
    nvs_cfg_load(&c);

    String p;
    p.reserve(WUI_RESERVE);
    p += WUI_HEAD;
    p += WUI_SUB_OPEN;
    add_status(p, c);
    p += WUI_SUB_END;
    p += WUI_RADIO_A;
    add_radio_fields(p, c);
    p += WUI_RADIO_B;
    p += WUI_CRYPTO;
    p += WUI_OTA;
    p += WUI_FACTORY;
    p += WUI_TAIL;
    send_page(s, p);
}

// 去掉首尾空白（空格/制表/CR/LF）：手机输入法与复制粘贴常带，会让"整串校验"误判非法
static String trim_ws(String s) {
    int b = 0;
    int e = (int)s.length();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substring(b, e);
}

// ---- 改参数 /set?freq=915.0&sf=7&bw=125&cr=5&power=20 ----
static void handle_set() {
    WebServer& s = *g_server;
    cfg_t c;
    nvs_cfg_load(&c);

    // 诊断：把本次收到的全部参数打到串口（哪一项坏了一看便知）
    Serial.printf("[web] /set 收到 %d 个参数：\n", s.args());
    for (int i = 0; i < s.args(); i++) {
        Serial.printf("[web]   %s='%s'\n", s.argName(i).c_str(), s.arg(i).c_str());
    }

    // 只接受合法值：全部经集中校验（与 CLI / 远程 FRAME_CFG 同一套规则），
    // 有一个非法就整表拒绝，不留半套参数被写进 NVS（比如 freq=0 会让射频彻底收不到）
    bool bad = false;
    String bad_key, bad_val;
    auto mark_bad = [&](const char* k, const char* v) { bad = true; bad_key = k; bad_val = v; };
    if (s.hasArg("freq_int") && s.hasArg("freq_frac")) {
        String vi = trim_ws(s.arg("freq_int"));
        String vf = trim_ws(s.arg("freq_frac"));
        double f = atof(vi.c_str()) + atof(vf.c_str());
        char fb[16];
        snprintf(fb, sizeof(fb), "%.2f", f);
        if (nvs_cfg_value_ok("freq", fb)) c.freq = (float)f;
        else mark_bad("freq_int+freq_frac", fb);
    }
    if (s.hasArg("sf")) {
        String vs = trim_ws(s.arg("sf"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("sf", v)) c.sf = (uint8_t)atoi(v);
        else mark_bad("sf", v);
    }
    if (s.hasArg("bw")) {
        String vs = trim_ws(s.arg("bw"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("bw", v)) c.bw = (uint8_t)atoi(v);
        else mark_bad("bw", v);
    }
    if (s.hasArg("cr")) {
        String vs = trim_ws(s.arg("cr"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("cr", v)) c.cr = (uint8_t)atoi(v);
        else mark_bad("cr", v);
    }
    if (s.hasArg("power")) {
        String vs = trim_ws(s.arg("power"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("power", v)) c.power = (int8_t)atoi(v);
        else mark_bad("power", v);
    }

    // 节点号 / 角色 / 自动维护延时（S6）——同样走集中校验，非法整表拒绝
    if (s.hasArg("node_id")) {
        String vs = trim_ws(s.arg("node_id"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("node_id", v)) c.node_id = (uint8_t)atoi(v);
        else mark_bad("node_id", v);
    }
    if (s.hasArg("role")) {
        String vs = trim_ws(s.arg("role"));
        const char* v = vs.c_str();
        if (*v == '\0')                       c.role = ROLE_UNSET;   // 空 = 未配置（允许回退）
        else if (nvs_cfg_value_ok("role", v)) c.role = role_from_name(v);
        else                                  mark_bad("role", v);
    }
    if (s.hasArg("auto_wifi_s")) {
        String vs = trim_ws(s.arg("auto_wifi_s"));
        const char* v = vs.c_str();
        if (nvs_cfg_value_ok("auto_wifi_s", v)) {       // 与 nvs_cfg_set 一致地"钳制"
            long l = atol(v);
            if (l < 15) l = 15;
            if (l > 60) l = 60;
            c.auto_wifi_s = (uint16_t)l;
        } else mark_bad("auto_wifi_s", v);
    }
    bool do_reboot = s.hasArg("reboot");

    if (bad) {
        String m = String("格式不正确：") + bad_key + " = \"" + bad_val + "\"";
        Serial.printf("[web] 拒绝保存：%s\n", m.c_str());
        send_done(s, true, m.c_str());
        return;
    }
    if (!nvs_cfg_pair_ok(c.sf, c.bw)) {
        send_done(s, true, "SF 与带宽不匹配：本机射频芯片 LLCC68 要求 BW125→SF≤9、BW250→≤10、BW500→≤11。");
        return;
    }
    if (!nvs_cfg_save(&c)) { send_done(s, true, "写入失败：NVS 保存出错。"); return; }
    if (do_reboot) {
        send_done(s, false, "已保存，正在重启…（约 3 秒后本页会断开，可重新连 WiFi）");
        delay(300);
        ESP.restart();
    }
    send_done(s, false, "参数已保存，重启后生效。");
}

// ---- 恢复出厂 /factory ----（提示后由用户在页面点“返回”）
static void handle_factory() {
    cfg_t c;
    nvs_cfg_factory(&c);
    if (!nvs_cfg_save(&c)) { send_done(*g_server, true, "写入失败：NVS 保存出错。"); return; }
    send_done(*g_server, false, "已恢复出厂默认（role 已清空、密钥已清除）。请断电重启后重新配置。");
}

// ---- 设密钥 /crypto?key=<32hex> 或 /crypto?off=1 ----
static void handle_crypto() {
    WebServer& s = *g_server;

    if (s.hasArg("off")) {
        crypto_disable();
        send_done(s, false, "加密已关闭，密钥已清除。");
        return;
    }

    if (!s.hasArg("key")) { send_done(s, true, "请输入正确的格式"); return; }

    // 先去掉空白再交给 crypto；判定标准与 crypto_set_key_hex 完全一致（恰好 32 个 hex）
    char k[40];
    size_t n = 0;
    for (const char* src = s.arg("key").c_str(); *src && n < sizeof(k) - 1; src++) {
        char ch = *src;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        k[n++] = ch;
    }
    k[n] = 0;

    if (!crypto_set_key_hex(k)) { send_done(s, true, "请输入正确的格式"); return; }
    send_done(s, false, "密钥已更新，立即生效。");
}

// ---- 刷固件：上传回调（分片写入 OTA 空闲分区）----
static void handle_upload() {
    WebServer& s = *g_server;
    HTTPUpload& up = s.upload();

    if (up.status == UPLOAD_FILE_START) {
        g_updating = true;
        g_upd_ok = false;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        g_updating = false;
        if (Update.end(true)) {
            g_upd_ok = true;
            send_done(s, false, "刷写成功，正在重启…");
            s.client().stop();
            delay(300);
            ESP.restart();
        } else {
            Update.printError(Serial);
        }
    }
}

// ---- 刷固件：完成回调。成功路径已在上面的 END 分支回过话并重启，
//      这里兜底「失败」和「压根没选文件」两种情况（否则浏览器一直转圈等不到回应）
static void handle_update_done() {
    if (g_upd_ok) return;
    send_done(*g_server, true, "刷写失败：请确认选择了完整的 .bin 固件。");
}

// ---- 退出维护模式 /exit ----
static void handle_exit() {
    WebServer& s = *g_server;
    send_done(s, false, "已退出维护模式，LoRa 数传恢复，WiFi 已关闭。");
    s.client().stop();
    delay(100);
    ota_stop();
}

// ---- ota_task：循环 handleClient；g_active 变 false 时自行清理并退出 ----
static void ota_task_func(void* arg) {
    (void)arg;
    while (g_active) {
        if (g_server) g_server->handleClient();

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

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // 任务自己清理（g_server 的唯一删除者，避免跨任务 use-after-free）
    if (g_server) { g_server->stop(); delete g_server; g_server = nullptr; }
    WiFi.mode(WIFI_OFF);
    radio_resume();
    g_ota_task = nullptr;
    Serial.println("[ota] 已退出维护模式，LoRa 恢复");
    vTaskDelete(nullptr);   // 删自己
}

// ---- 上电自动进入维护模式（条件式，用户 2026-10-17 定）----
// 判据："本次开机以来空口上没有任何活动"。用现成信号，不新增计数：
//   mesh_rx_count()       —— 收到过有效帧
//   mesh_drop_crc_count() —— 收到过 CRC 错的帧（说明空口上确实有人在发）
//   route_neighbors()     —— 邻居表非空
//   mav_router_heartbeat_ok() —— 有 MAVLink 心跳（机载时飞控连着，不该开 AP）
static bool g_auto_done = false;   // 本次开机是否已处理过（自动进入只做一次）

void ota_auto_tick(void) {
    if (g_auto_done || g_active) return;

    cfg_t c;
    uint16_t delay_s = 30;
    // 注意：nvs_cfg_load() 的返回值表示"role 是否已设置"，与这里的延时无关 ——
    // 未配置角色正是最需要自动进维护模式的场景，所以不能拿返回值当条件。
    (void)nvs_cfg_load(&c);
    if (c.auto_wifi_s >= 15 && c.auto_wifi_s <= 60) delay_s = c.auto_wifi_s;

    if (millis() < (uint32_t)delay_s * 1000u) return;

    neighbor_t nb[1];
    bool activity = (mesh_rx_count() > 0) || (mesh_drop_crc_count() > 0)
                 || (route_neighbors(nb, 1) > 0)
                 || mav_router_heartbeat_ok();

    g_auto_done = true;            // 无论结果都只处理一次
    if (activity) {
        Serial.println("[ota] 上电后有链路活动 → 不自动进入 WiFi 维护模式");
        return;
    }
    Serial.printf("[ota] 上电 %us 无任何链路活动 → 自动进入 WiFi 维护模式\n", (unsigned)delay_s);
    ota_start();
}

void ota_start() {
    if (g_active) return;

    // 1. 暂停 LoRa（WiFi 与 LoRa 二选一）
    radio_suspend();

    // 2. 读 node_id 定 SSID
    cfg_t c;
    nvs_cfg_load(&c);
    if (c.node_id) g_node_id = c.node_id;

    char ssid[32];
    if (c.wifi_ssid[0]) snprintf(ssid, sizeof(ssid), "%s", c.wifi_ssid);
    else                snprintf(ssid, sizeof(ssid), "EMlink-%u", g_node_id);

    // 3. 起 AP（密码可为空）
    WiFi.mode(WIFI_AP);
    if (c.wifi_pass[0]) WiFi.softAP(ssid, c.wifi_pass);
    else                WiFi.softAP(ssid);
    IPAddress ip(192, 168, 4, 1);
    IPAddress gw(192, 168, 4, 1);
    IPAddress mask(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gw, mask);

    // 4. WebServer + 路由
    g_server = new WebServer(80);
    g_server->on("/",        handle_root);
    g_server->on("/set",     handle_set);
    g_server->on("/crypto",  handle_crypto);
    g_server->on("/factory", HTTP_POST, handle_factory);
    g_server->on("/update",  HTTP_POST, handle_update_done, handle_upload);
    g_server->on("/exit",    handle_exit);
    g_server->begin();

    g_active = true;

    g_ap_start_ms    = millis();
    g_last_client_ms = millis();
    g_updating       = false;

    // 5. 起任务（core1）
    xTaskCreatePinnedToCore(ota_task_func, "ota", OTA_TASK_STACK,
                            nullptr, OTA_TASK_PRIO, &g_ota_task, OTA_TASK_CORE);

    Serial.printf("[ota] AP %s @ 192.168.4.1\n", ssid);
}

void ota_stop() {
    if (!g_active) return;
    g_active = false;   // 通知 ota_task 退出；它自行清理 g_server / WiFi / radio
}

bool ota_active() { return g_active; }

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
