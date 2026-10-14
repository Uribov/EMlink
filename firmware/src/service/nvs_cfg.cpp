// ============================================================
// nvs_cfg.cpp —— NVS 配置读写（掉电不丢）
// 依赖 Preferences；约定见架构 §9。
// ============================================================
#include "nvs_cfg.h"
#include <string.h>
#include <stdlib.h>

static const char* NVS_NS = "mesh";

// ---- 默认值（架构 §9）----
void nvs_cfg_factory(cfg_t* out) {
    if (!out) return;
    out->role           = ROLE_UNSET;
    out->node_id        = 0;        // 0 = 自动分配
    out->sysid          = 0;        // 0 = 按角色默认
    out->freq           = 915.0f;
    out->sf             = 7;
    out->bw             = 125;
    out->cr             = 5;        // 4/5
    out->power          = 20;
    out->syncword       = 0x12;
    out->uart_baud      = 115200;
    out->ttl            = 5;
    out->ack_timeout_ms = 300;
    out->retries        = 3;
    out->beacon_s       = 1;
    out->failsafe_s     = 3;
    memset(out->name, 0, sizeof(out->name));
    memset(out->crypto_key, 0, sizeof(out->crypto_key));
    memset(out->wifi_ssid, 0, sizeof(out->wifi_ssid));
    memset(out->wifi_pass, 0, sizeof(out->wifi_pass));
    memset(out->ota_url, 0, sizeof(out->ota_url));
}

// 加载；无配置则写默认。返回 false = role 未设（应强制进 CLI）
bool nvs_cfg_load(cfg_t* out) {
    if (!out) return false;
    Preferences p;
    if (!p.begin(NVS_NS, true)) { nvs_cfg_factory(out); return false; }

    bool has_role = p.isKey("role");
    out->role           = p.getUChar("role", ROLE_UNSET);
    out->node_id        = p.getUChar("node_id", 0);
    out->sysid          = p.getUChar("sysid", 0);
    out->freq           = p.getFloat("freq", 915.0f);
    out->sf             = p.getUChar("sf", 7);
    out->bw             = p.getUChar("bw", 125);
    out->cr             = p.getUChar("cr", 5);
    out->power          = p.getChar("power", 20);
    out->syncword       = p.getUShort("syncword", 0x12);
    out->uart_baud      = p.getUInt("uart_baud", 115200);
    out->ttl            = p.getUChar("ttl", 5);
    out->ack_timeout_ms = p.getUInt("ack_timeout_ms", 300);
    out->retries        = p.getUChar("retries", 3);
    out->beacon_s       = p.getUShort("beacon_s", 1);
    out->failsafe_s     = p.getUShort("failsafe_s", 3);
    p.getString("name", out->name, sizeof(out->name));
    memset(out->crypto_key, 0, sizeof(out->crypto_key));   // 默认关闭
    if (p.getBytesLength("crypto_key") == 16) p.getBytes("crypto_key", out->crypto_key, 16);
    p.getString("wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    p.getString("wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    p.getString("ota_url", out->ota_url, sizeof(out->ota_url));
    p.end();
    return has_role;
}

bool nvs_cfg_save(const cfg_t* in) {
    if (!in) return false;
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    p.putUChar("role", in->role);
    p.putUChar("node_id", in->node_id);
    p.putUChar("sysid", in->sysid);
    p.putFloat("freq", in->freq);
    p.putUChar("sf", in->sf);
    p.putUChar("bw", in->bw);
    p.putUChar("cr", in->cr);
    p.putChar("power", in->power);
    p.putUShort("syncword", in->syncword);
    p.putUInt("uart_baud", in->uart_baud);
    p.putUChar("ttl", in->ttl);
    p.putUInt("ack_timeout_ms", in->ack_timeout_ms);
    p.putUChar("retries", in->retries);
    p.putUShort("beacon_s", in->beacon_s);
    p.putUShort("failsafe_s", in->failsafe_s);
    p.putString("name", in->name);
    p.putBytes("crypto_key", in->crypto_key, 16);
    p.putString("wifi_ssid", in->wifi_ssid);
    p.putString("wifi_pass", in->wifi_pass);
    p.putString("ota_url", in->ota_url);
    p.end();
    return true;
}

// ---- 单项读写（CLI set/get）----

// 字符串 -> role
static uint8_t role_from_str(const char* s) {
    if (!s) return ROLE_UNSET;
    if (strcmp(s, "air") == 0)   return ROLE_AIR;
    if (strcmp(s, "gcs") == 0)   return ROLE_GCS;
    if (strcmp(s, "relay") == 0) return ROLE_RELAY;
    return ROLE_UNSET;
}
static const char* role_to_str(uint8_t r) {
    switch (r) {
        case ROLE_AIR:   return "air";
        case ROLE_GCS:   return "gcs";
        case ROLE_RELAY: return "relay";
        default:         return "unset";
    }
}

bool nvs_cfg_set(const char* key, const char* val) {
    if (!key || !val) return false;
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    bool ok = true;

    if (strcmp(key, "role") == 0) {
        p.putUChar("role", role_from_str(val));
    } else if (strcmp(key, "node_id") == 0) {
        p.putUChar("node_id", (uint8_t)atoi(val));
    } else if (strcmp(key, "sysid") == 0) {
        p.putUChar("sysid", (uint8_t)atoi(val));
    } else if (strcmp(key, "freq") == 0) {
        p.putFloat("freq", atof(val));
    } else if (strcmp(key, "sf") == 0) {
        p.putUChar("sf", (uint8_t)atoi(val));
    } else if (strcmp(key, "bw") == 0) {
        p.putUChar("bw", (uint8_t)atoi(val));
    } else if (strcmp(key, "cr") == 0) {
        p.putUChar("cr", (uint8_t)atoi(val));
    } else if (strcmp(key, "power") == 0) {
        p.putChar("power", (int8_t)atoi(val));
    } else if (strcmp(key, "syncword") == 0) {
        p.putUShort("syncword", (uint16_t)strtoul(val, nullptr, 0));
    } else if (strcmp(key, "uart_baud") == 0) {
        p.putUInt("uart_baud", (uint32_t)atol(val));
    } else if (strcmp(key, "ttl") == 0) {
        p.putUChar("ttl", (uint8_t)atoi(val));
    } else if (strcmp(key, "ack_timeout_ms") == 0) {
        p.putUInt("ack_timeout_ms", (uint32_t)atol(val));
    } else if (strcmp(key, "retries") == 0) {
        p.putUChar("retries", (uint8_t)atoi(val));
    } else if (strcmp(key, "beacon_s") == 0) {
        p.putUShort("beacon_s", (uint16_t)atoi(val));
    } else if (strcmp(key, "failsafe_s") == 0) {
        p.putUShort("failsafe_s", (uint16_t)atoi(val));
    } else if (strcmp(key, "name") == 0) {
        p.putString("name", val);
    } else {
        ok = false;   // 未知 key
    }
    p.end();
    return ok;
}

bool nvs_cfg_get(const char* key, char* val, size_t len) {
    if (!key || !val || len == 0) return false;
    Preferences p;
    if (!p.begin(NVS_NS, true)) return false;
    bool ok = true;
    val[0] = '\0';

    if (strcmp(key, "role") == 0) {
        snprintf(val, len, "%s", role_to_str(p.getUChar("role", ROLE_UNSET)));
    } else if (strcmp(key, "node_id") == 0) {
        snprintf(val, len, "%u", p.getUChar("node_id", 0));
    } else if (strcmp(key, "sysid") == 0) {
        snprintf(val, len, "%u", p.getUChar("sysid", 0));
    } else if (strcmp(key, "freq") == 0) {
        snprintf(val, len, "%.1f", p.getFloat("freq", 915.0f));
    } else if (strcmp(key, "sf") == 0) {
        snprintf(val, len, "%u", p.getUChar("sf", 7));
    } else if (strcmp(key, "bw") == 0) {
        snprintf(val, len, "%u", p.getUChar("bw", 125));
    } else if (strcmp(key, "cr") == 0) {
        snprintf(val, len, "%u", p.getUChar("cr", 5));
    } else if (strcmp(key, "power") == 0) {
        snprintf(val, len, "%d", p.getChar("power", 20));
    } else if (strcmp(key, "syncword") == 0) {
        snprintf(val, len, "%u", p.getUShort("syncword", 0x12));
    } else if (strcmp(key, "uart_baud") == 0) {
        snprintf(val, len, "%u", p.getUInt("uart_baud", 115200));
    } else if (strcmp(key, "ttl") == 0) {
        snprintf(val, len, "%u", p.getUChar("ttl", 5));
    } else if (strcmp(key, "ack_timeout_ms") == 0) {
        snprintf(val, len, "%u", p.getUInt("ack_timeout_ms", 300));
    } else if (strcmp(key, "retries") == 0) {
        snprintf(val, len, "%u", p.getUChar("retries", 3));
    } else if (strcmp(key, "beacon_s") == 0) {
        snprintf(val, len, "%u", p.getUShort("beacon_s", 1));
    } else if (strcmp(key, "failsafe_s") == 0) {
        snprintf(val, len, "%u", p.getUShort("failsafe_s", 3));
    } else if (strcmp(key, "name") == 0) {
        p.getString("name", val, len);
    } else {
        ok = false;
    }
    p.end();
    return ok;
}
