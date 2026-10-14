// ============================================================
// ota.cpp —— OTA 固件升级 / WiFi Web 管理
// WiFi AP + WebServer + 内嵌 HTML；改参数 / 设密钥 / 刷固件(Update.h)。
// 进入维护模式 radio_suspend()（WiFi 与 LoRa 二选一），退出恢复。
// ============================================================
#include "ota.h"
#include "../rtos.h"
#include "nvs_cfg.h"
#include "crypto.h"
#include "../drivers/radio.h"
#include "../app/role.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <string.h>
#include <stdio.h>

static WebServer*   g_server = nullptr;
static TaskHandle_t g_ota_task = nullptr;
static volatile bool g_active = false;
static uint8_t      g_node_id = 1;

// 角色号 → 字符串（网页显示用；与 nvs_cfg 内部约定一致）
static const char* role_name(uint8_t r) {
    switch (r) {
        case ROLE_AIR:   return "air";
        case ROLE_GCS:   return "gcs";
        case ROLE_RELAY: return "relay";
        default:         return "unset";
    }
}

// ---- 内嵌 HTML（单页，含改参数/设密钥/刷固件三块）----
static const char PAGE_HEAD[] PROGMEM =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>EMlink 配置</title><style>"
"body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#111;color:#eee}"
"h1{font-size:20px}h2{font-size:16px;margin-top:24px;border-bottom:1px solid #333;padding-bottom:4px}"
"label{display:block;margin:8px 0 2px;font-size:13px;color:#aaa}"
"input,select{width:100%;box-sizing:border-box;padding:8px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;font-size:14px}"
"button{display:block;width:100%;margin:12px 0;padding:10px;background:#2a7;color:#fff;border:0;border-radius:4px;font-size:15px}"
"button.off{background:#a33}button.ota{background:#37c}"
".hint{font-size:12px;color:#888;margin-top:-6px}"
"</style></head><body>";

static const char PAGE_TAIL[] PROGMEM =
"</body></html>";

static const char* page_head() { return PAGE_HEAD; }
static const char* page_tail() { return PAGE_TAIL; }

// 打印一段字符串（PROGMEM）
static void ws_send_p(WebServer& s, const char* p) {
    while (pgm_read_byte(p)) s.client().write((char)pgm_read_byte(p++));
}

static void ws_send_c(WebServer& s, const char* c) {
    s.client().write(c, strlen(c));
}

// ---- 主页 ----
static void handle_root() {
    WebServer& s = *g_server;
    s.sendHeader("Content-Type", "text/html; charset=utf-8");
    s.client().print("HTTP/1.1 200 OK\r\n");

    ws_send_p(s, page_head());

    char buf[160];
    cfg_t c;
    nvs_cfg_load(&c);
    snprintf(buf, sizeof(buf),
        "<h1>EMlink 数传配置</h1>"
        "<div class='hint'>节点 id=%u | 角色=%s | 加密=%s</div>",
        c.node_id ? c.node_id : (unsigned)g_node_id,
        role_name(c.role),
        crypto_enabled() ? "开" : "关");
    ws_send_c(s, buf);

    // ---- 改参数表单 ----
    ws_send_p(s, PSTR("<h2>射频参数</h2>"
        "<form action='/set' method='get'>"
        "<label>频率 freq (MHz)</label><input name='freq' value='915.0'>"
        "<label>扩频 sf (7~11)</label><input name='sf' value='7'>"
        "<label>带宽 bw (kHz)</label><input name='bw' value='125'>"
        "<label>编码率 cr (5~8)</label><input name='cr' value='5'>"
        "<label>功率 power (dBm)</label><input name='power' value='20'>"
        "<button type='submit'>保存参数（重启生效）</button></form>"));

    // ---- 设密钥表单 ----
    ws_send_p(s, PSTR("<h2>加密密钥</h2>"
        "<form action='/crypto' method='get'>"
        "<label>密钥（32 个 hex 字符）</label><input name='key' maxlength='32' placeholder='0123456789abcdef0123456789abcdef'>"
        "<button type='submit'>设置密钥并启用</button></form>"
        "<form action='/crypto' method='get'>"
        "<button class='off' name='off' value='1' type='submit'>关闭加密</button></form>"));

    // ---- 刷固件表单 ----
    ws_send_p(s, PSTR("<h2>刷固件</h2>"
        "<form action='/update' method='post' enctype='multipart/form-data'>"
        "<label>选择 .bin 固件</label><input type='file' name='firmware'>"
        "<button class='ota' type='submit'>上传刷写</button></form>"
        "<div class='hint'>刷写成功自动重启切到新分区</div>"));

    // ---- 退出维护模式 ----
    ws_send_p(s, PSTR("<h2>系统</h2>"
        "<form action='/exit' method='get'>"
        "<button class='off' type='submit'>退出维护模式（恢复数传）</button></form>"));

    ws_send_p(s, page_tail());
    s.client().stop();
}

// ---- 改参数 /set?freq=915&sf=7... ----
static void handle_set() {
    WebServer& s = *g_server;
    cfg_t c;
    nvs_cfg_load(&c);

    if (s.hasArg("freq"))  c.freq  = atof(s.arg("freq").c_str());
    if (s.hasArg("sf"))    c.sf    = (uint8_t)atoi(s.arg("sf").c_str());
    if (s.hasArg("bw"))    c.bw    = (uint8_t)atoi(s.arg("bw").c_str());
    if (s.hasArg("cr"))    c.cr    = (uint8_t)atoi(s.arg("cr").c_str());
    if (s.hasArg("power")) c.power = (int8_t)atoi(s.arg("power").c_str());
    nvs_cfg_save(&c);

    s.sendHeader("Content-Type", "text/html; charset=utf-8");
    s.client().print("HTTP/1.1 200 OK\r\n");
    ws_send_p(s, page_head());
    ws_send_p(s, PSTR("<h1>已保存</h1><div class='hint'>参数已写入，重启后生效。</div>"
        "<form action='/' method='get'><button type='submit'>返回</button></form>"));
    ws_send_p(s, page_tail());
    s.client().stop();
}

// ---- 设密钥 /crypto?key=<32hex> 或 /crypto?off=1 ----
static void handle_crypto() {
    WebServer& s = *g_server;
    bool ok = false;
    if (s.hasArg("off")) {
        crypto_disable();
        ok = true;
    } else if (s.hasArg("key")) {
        ok = crypto_set_key_hex(s.arg("key").c_str());
    }

    s.sendHeader("Content-Type", "text/html; charset=utf-8");
    s.client().print("HTTP/1.1 200 OK\r\n");
    ws_send_p(s, page_head());
    if (ok) ws_send_p(s, PSTR("<h1>密钥已更新</h1><div class='hint'>立即生效。</div>"));
    else    ws_send_p(s, PSTR("<h1>失败</h1><div class='hint'>密钥必须是 32 个 hex 字符。</div>"));
    ws_send_p(s, PSTR("<form action='/' method='get'><button type='submit'>返回</button></form>"));
    ws_send_p(s, page_tail());
    s.client().stop();
}

// ---- 刷固件 /update（multipart 上传，Update.h 写 ota_1 后重启）----
static void handle_update() {
    WebServer& s = *g_server;
    HTTPUpload& upload = s.upload();

    if (upload.status == UPLOAD_FILE_START) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            // 成功：回一句，稍后重启切分区
            s.sendHeader("Content-Type", "text/html; charset=utf-8");
            s.client().print("HTTP/1.1 200 OK\r\n");
            ws_send_p(s, PSTR("<h1>刷写成功</h1><div class='hint'>正在重启…</div>"));
            s.client().stop();
            delay(300);
            ESP.restart();
            return;
        } else {
            Update.printError(Serial);
        }
    }
}

// ---- 退出维护模式 /exit ----
static void handle_exit() {
    WebServer& s = *g_server;
    s.sendHeader("Content-Type", "text/html; charset=utf-8");
    s.client().print("HTTP/1.1 200 OK\r\n");
    ws_send_p(s, PSTR("<h1>已退出</h1><div class='hint'>LoRa 已恢复，WiFi 已关闭。</div>"));
    s.client().stop();
    delay(100);
    ota_stop();
}

// ---- ota_task：循环 handleClient；g_active 变 false 时自行清理并退出 ----
static void ota_task_func(void* arg) {
    (void)arg;
    while (g_active) {
        if (g_server) g_server->handleClient();
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
    g_server->on("/update",  HTTP_POST, [](){}, handle_update);
    g_server->on("/exit",    handle_exit);
    g_server->begin();

    g_active = true;

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

void ota_handle() {
    if (g_server) g_server->handleClient();
}
