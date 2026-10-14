// ============================================================
// nvs_cfg.cpp —— NVS 配置读写（掉电不丢）
// 依赖 Preferences；约定见架构 §9。
// ============================================================
#include "nvs_cfg.h"
#include "../pinmap.h"      // 硬件能力宏（RADIO_FREQ_* / RADIO_PWR_*）
#include <esp_mac.h>        // esp_read_mac（node_id 自动分配）
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

// ---- 集中校验 ----

// 字符串 -> 整数，必须整个串都是数字（endptr 要走到串尾，拒绝 "12abc"）
static bool parse_long(const char* s, long* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

// 字符串 -> 浮点，同样校验尾部
static bool parse_double(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

// 取值范围校验：与 nvs_cfg 支持的全部键一一对应，未知键一律拒绝
bool nvs_cfg_value_ok(const char* key, const char* val) {
    if (!key || !val) return false;

    if (strcmp(key, "role") == 0) {
        return strcmp(val, "air") == 0 || strcmp(val, "gcs") == 0 || strcmp(val, "relay") == 0;
    }

    long l;
    double d;

    if (strcmp(key, "node_id") == 0) {
        return parse_long(val, &l) && l >= 1 && l <= 254;
    }
    if (strcmp(key, "sysid") == 0) {
        return parse_long(val, &l) && l >= 0 && l <= 255;
    }
    if (strcmp(key, "freq") == 0) {
        return parse_double(val, &d) && d >= RADIO_FREQ_MIN_MHZ && d <= RADIO_FREQ_MAX_MHZ;
    }
    if (strcmp(key, "sf") == 0) {
        return parse_long(val, &l) && l >= 7 && l <= 11;
    }
    if (strcmp(key, "bw") == 0) {
        return parse_long(val, &l) && (l == 125 || l == 250 || l == 500);
    }
    if (strcmp(key, "cr") == 0) {
        return parse_long(val, &l) && l >= 5 && l <= 8;
    }
    if (strcmp(key, "power") == 0) {
        return parse_long(val, &l) && l >= RADIO_PWR_MIN_DBM && l <= RADIO_PWR_MAX_DBM;
    }
    if (strcmp(key, "syncword") == 0) {
        return parse_long(val, &l) && l >= 0 && l <= 65535;
    }
    if (strcmp(key, "uart_baud") == 0) {
        static const long BAUD[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
        if (!parse_long(val, &l)) return false;
        for (size_t i = 0; i < sizeof(BAUD) / sizeof(BAUD[0]); i++) {
            if (l == BAUD[i]) return true;
        }
        return false;
    }
    if (strcmp(key, "ttl") == 0) {
        return parse_long(val, &l) && l >= 1 && l <= 10;
    }
    if (strcmp(key, "ack_timeout_ms") == 0) {
        return parse_long(val, &l) && l >= 50 && l <= 10000;
    }
    if (strcmp(key, "retries") == 0) {
        return parse_long(val, &l) && l >= 0 && l <= 10;
    }
    if (strcmp(key, "beacon_s") == 0) {
        return parse_long(val, &l) && l >= 1 && l <= 60;
    }
    if (strcmp(key, "failsafe_s") == 0) {
        return parse_long(val, &l) && l >= 1 && l <= 60;
    }
    if (strcmp(key, "name") == 0) {
        size_t n = strlen(val);
        return n >= 1 && n <= 15;
    }
    return false;   // 未知键一律拒绝
}

// 从芯片 MAC 派生 1..254 节点号：同板稳定、不同板大概率不同，避免全网默认 1 号撞号
uint8_t nvs_cfg_assign_node_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return (uint8_t)(1 + ((mac[3] ^ mac[4] ^ mac[5]) % 254));   // 1..254
}

// 开机调用一次：读持久化世代号 → +1 → 写回 NVS → 返回本次开机要用的 epoch。
// begin 失败也返回递增后的值（宁可用新值，不冒 nonce 复用风险）。
uint8_t nvs_cfg_next_enc_epoch(void) {
    Preferences p;
    uint8_t v = 0;
    if (p.begin(NVS_NS, false)) {
        v = (uint8_t)(p.getUChar("enc_epoch", 0) + 1);
        p.putUChar("enc_epoch", v);
        p.end();
    } else {
        v = 1;
    }
    return v;
}

bool nvs_cfg_set(const char* key, const char* val) {
    if (!key || !val) return false;
    // 集中校验：任何入口（CLI / 远程 FRAME_CFG / 网页）都不准把非法值写进 NVS，
    // 否则重启后射频可能起不来（如 freq=0、sf=99、power=30）。
    if (!nvs_cfg_value_ok(key, val)) return false;
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

// 应用 "key=value"（远程参数同步）；白名单拒 role
bool nvs_cfg_apply_kv(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > 63) return false;

    // 拷到可写缓冲，找 '=' 切 key/value
    char buf[64];
    memcpy(buf, data, len);
    buf[len] = '\0';

    char* eq = strchr(buf, '=');
    if (!eq) return false;
    *eq = '\0';
    const char* key = buf;
    const char* val = eq + 1;
    if (key[0] == '\0' || val[0] == '\0') return false;

    // 白名单：role 不允许远程改（误操作直接断链）
    if (strcmp(key, "role") == 0) return false;

    return nvs_cfg_set(key, val);
}
