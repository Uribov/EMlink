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

// 整页 String 预留容量（还原实测主页 9276 字节，留 ~1KB 余量；
// 一次分配，避免反复 realloc 造成堆碎片）
#define WUI_RESERVE 10240

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
    char b[288];

    snprintf(b, sizeof(b),
             "<div><label for=\"f\">频率 MHz</label>"
             "<input id=\"f\" name=\"freq\" type=\"text\" value=\"%.1f\"></div>"
             "<div><label for=\"sf\">扩频因子</label><select id=\"sf\" name=\"sf\">",
             (double)c.freq);
    p += b;
    add_opts(p, SFV, 5, c.sf);

    p += "</select></div><div><label for=\"bw\">带宽 kHz</label><select id=\"bw\" name=\"bw\">";
    add_opts(p, BWV, 3, c.bw);

    p += "</select></div><div><label for=\"cr\">编码率</label><select id=\"cr\" name=\"cr\">";
    add_opts(p, CRV, 4, c.cr);

    snprintf(b, sizeof(b),
             "</select></div><div class=\"wide\"><label for=\"pw\">发射功率 dBm</label>"
             "<input id=\"pw\" name=\"power\" type=\"text\" value=\"%d\"></div>",
             (int)c.power);
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

// ---- 改参数 /set?freq=915.0&sf=7&bw=125&cr=5&power=20 ----
static void handle_set() {
    WebServer& s = *g_server;
    cfg_t c;
    nvs_cfg_load(&c);

    // 只接受合法值：全部经集中校验（与 CLI / 远程 FRAME_CFG 同一套规则），
    // 有一个非法就整表拒绝，不留半套参数被写进 NVS（比如 freq=0 会让射频彻底收不到）
    bool bad = false;
    if (s.hasArg("freq"))  { const char* v = s.arg("freq").c_str();  if (nvs_cfg_value_ok("freq", v))  c.freq  = (float)atof(v); else bad = true; }
    if (s.hasArg("sf"))    { const char* v = s.arg("sf").c_str();    if (nvs_cfg_value_ok("sf", v))    c.sf    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("bw"))    { const char* v = s.arg("bw").c_str();    if (nvs_cfg_value_ok("bw", v))    c.bw    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("cr"))    { const char* v = s.arg("cr").c_str();    if (nvs_cfg_value_ok("cr", v))    c.cr    = (uint8_t)atoi(v); else bad = true; }
    if (s.hasArg("power")) { const char* v = s.arg("power").c_str(); if (nvs_cfg_value_ok("power", v)) c.power = (int8_t)atoi(v); else bad = true; }

    if (bad) { send_done(s, true, "请输入正确的格式"); return; }
    if (!nvs_cfg_save(&c)) { send_done(s, true, "写入失败：NVS 保存出错。"); return; }
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
