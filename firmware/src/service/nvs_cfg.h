#pragma once
// ============================================================
// nvs_cfg —— NVS 配置读写（掉电不丢）
// 依赖：Preferences（Arduino NVS 封装）；约定见架构 §9
// ============================================================
#include <Arduino.h>
#include <Preferences.h>

// 角色
enum : uint8_t { ROLE_UNSET = 0, ROLE_AIR = 1, ROLE_GCS = 2, ROLE_RELAY = 3 };

// 配置结构（内存镜像）
struct cfg_t {
    uint8_t  role;            // ROLE_*
    uint8_t  node_id;         // 1~254
    uint8_t  sysid;           // AIR=飞控号，GCS=255
    float    freq;            // 915.0
    uint8_t  sf;              // 7
    uint8_t  bw;              // 125 kHz
    uint8_t  cr;              // 5 (4/5)
    int8_t   power;           // 20 dBm
    uint16_t syncword;        // 0x12
    uint32_t uart_baud;       // 115200
    uint8_t  ttl;             // 5
    uint32_t ack_timeout_ms;  // 300
    uint8_t  retries;         // 3
    uint16_t beacon_s;        // 1
    uint16_t failsafe_s;      // 3
    char     name[16];
    uint8_t  crypto_key[16];  // AES-128 密钥（全 0 = 加密关闭）
    char     wifi_ssid[32];   // WiFi AP 名（空 = 用 EMlink-<node_id>）
    char     wifi_pass[32];   // WiFi AP 密码（空 = 无密码）
    char     ota_url[96];     // 预留：将来从 URL 拉固件（当前网页上传，不用）
};

// 加载；无配置则写默认。返回 false = role 未设（应强制进 CLI）
bool nvs_cfg_load(cfg_t* out);
// 保存
bool nvs_cfg_save(const cfg_t* in);
// 恢复出厂默认
void nvs_cfg_factory(cfg_t* out);
// 单项读写（CLI 的 set/get 用）
bool nvs_cfg_set(const char* key, const char* val);
bool nvs_cfg_get(const char* key, char* val, size_t len);

// 应用一条 "key=value"（FRAME_CFG 远程参数同步用）
// 白名单：role 拒绝远程改（防误操作断链）；其余走 nvs_cfg_set
// 返回 false = 格式错 / 未知 key / 被白名单拦截
bool nvs_cfg_apply_kv(const uint8_t* data, size_t len);
