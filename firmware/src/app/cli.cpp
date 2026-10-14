// ============================================================
// cli.cpp —— USB CDC1 命令行
// 依赖 usb_cdc.h / nvs_cfg.h / crypto.h / stats.h；命令约定见架构 §9
// ============================================================
#include "cli.h"
#include "../rtos.h"
#include "../drivers/usb_cdc.h"
#include "../drivers/radio.h"
#include "../drivers/uart_fc.h"
#include "../net/mesh.h"
#include "../net/route.h"
#include "../service/nvs_cfg.h"
#include "../service/crypto.h"
#include "../service/ota.h"
#include "../service/mav_router.h"
#include "../service/frag.h"
#include "../service/addr_map.h"
#include "../service/version.h"
#include "../app/stats.h"
#include "../link/duty.h"
#include "../link/adr.h"
#include "../service/ratelimit.h"
#include <string.h>

static TaskHandle_t g_cli_task = nullptr;
static char g_line[128];
static size_t g_line_len = 0;

static void cli_print(const char* s) {
    usb_cli_write((const uint8_t*)s, strlen(s));
}

void cli_banner() {
    cli_print("\r\n===== LoRa Mesh 数传 CLI =====\r\n输入 help 看命令\r\n> ");
}

void cli_help() {
    cli_print("命令：\r\n"
              "  role air|gcs|relay   设置角色\r\n"
              "  set <键> <值>        改配置（sf/power/ttl/...）\r\n"
              "  get [键]             读配置（无键打印全部）\r\n"
              "  save                 保存\r\n"
              "  reset                重启\r\n"
              "  factory              恢复出厂默认（清 role/密钥/参数）\r\n"
              "  status               看状态\r\n"
              "  stats                看统计\r\n"
              "  crypto <32hex>|off   设/关加密密钥（空=看状态）\r\n"
              "  wifi [off]           开/关 WiFi 维护模式\r\n"
              "  scan                 扫邻居 + 信号\r\n"
              "  ping <node_id>       测通断/RTT\r\n"
              "  cfg <键> <值>        广播参数给全网（GCS）\r\n"
              "  ver                  看固件/协议版本\r\n"
              "  help                 帮助\r\n");
}

static void cmd_get_all() {
    const char* keys[] = {"role","node_id","sysid","freq","sf","bw","cr","power",
                          "syncword","uart_baud","ttl","ack_timeout_ms","retries",
                          "beacon_s","failsafe_s","name"};
    char val[64];
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        if (nvs_cfg_get(keys[i], val, sizeof(val))) {
            char line[96];
            snprintf(line, sizeof(line), "  %-14s %s\r\n", keys[i], val);
            cli_print(line);
        }
    }
}

void cli_handle_line(const char* line) {
    // 分词
    char cmd[32] = {0}, arg1[32] = {0}, arg2[64] = {0};
    int n = sscanf(line, "%31s %31s %63s", cmd, arg1, arg2);

    if (n < 1) return;

    if (strcmp(cmd, "help") == 0) {
        cli_help();
    } else if (strcmp(cmd, "role") == 0) {
        if (n >= 2 && nvs_cfg_set("role", arg1)) {
            cli_print("角色已设为 ");
            cli_print(arg1);
            cli_print("（save 后重启生效）\r\n");
        } else {
            cli_print("用法：role air|gcs|relay\r\n");
        }
    } else if (strcmp(cmd, "set") == 0) {
        if (n >= 3 && nvs_cfg_set(arg1, arg2)) {
            cli_print("已设置（save 后生效）\r\n");
        } else {
            cli_print("设置失败：键名未知或值超出范围（用 get 查看当前值）\r\n");
        }
    } else if (strcmp(cmd, "get") == 0) {
        if (n >= 2) {
            char val[64];
            if (nvs_cfg_get(arg1, val, sizeof(val))) {
                char line[96];
                snprintf(line, sizeof(line), "%s = %s\r\n", arg1, val);
                cli_print(line);
            } else {
                cli_print("未知键\r\n");
            }
        } else {
            cmd_get_all();
        }
    } else if (strcmp(cmd, "save") == 0) {
        cli_print("已保存\r\n");
        if (evg) xEventGroupSetBits(evg, EV_CFG_CHANGED);
    } else if (strcmp(cmd, "reset") == 0) {
        cli_print("重启中...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(cmd, "factory") == 0) {
        cfg_t c;
        nvs_cfg_factory(&c);
        if (nvs_cfg_save(&c)) cli_print("已恢复出厂默认（role 已清空、密钥已清除）——重启后进入配置模式\r\n");
        else                  cli_print("恢复失败：NVS 写入错误\r\n");
    } else if (strcmp(cmd, "status") == 0) {
        char val[64];
        char line[128];
        nvs_cfg_get("role", val, sizeof(val));
        snprintf(line, sizeof(line), "role=%s\r\n", val);
        cli_print(line);
        nvs_cfg_get("node_id", val, sizeof(val));
        snprintf(line, sizeof(line), "node_id=%s  撞号冲突=%u\r\n", val, (unsigned)mesh_id_conflict_count());
        cli_print(line);
        snprintf(line, sizeof(line), "sysid=%u（本机 MAVLink 号）  sysid撞号=%u\r\n",
                 (unsigned)mav_router_local_sysid(), (unsigned)mav_router_sysid_conflict_count());
        cli_print(line);
        nvs_cfg_get("beacon_s", val, sizeof(val));
        snprintf(line, sizeof(line), "beacon_s=%s\r\n", val);
        cli_print(line);
        nvs_cfg_get("name", val, sizeof(val));
        snprintf(line, sizeof(line), "name=%s\r\n", val[0] ? val : "-");
        cli_print(line);
        snprintf(line, sizeof(line), "RF: %s\r\n",
                 radio_is_ready() ? "就绪" : "初始化失败（每 10s 重试，LED 红灯）");
        cli_print(line);
        snprintf(line, sizeof(line), "占空比: 超限放行=%u 次  限流丢弃=%u 次（控制类 %u，超桶放行 %u）  遥测降频=%u\r\n",
                 (unsigned)duty_oversize_count(), (unsigned)ratelimit_drop_count(),
                 (unsigned)ratelimit_ctrl_drop_count(), (unsigned)ratelimit_oversize_count(),
                 (unsigned)mav_router_stream_drop_count());
        cli_print(line);
        snprintf(line, sizeof(line), "认证失败: %u 帧（只影响本机投递，不影响转发）\r\n",
                 (unsigned)mesh_drop_auth_count());
        cli_print(line);
        snprintf(line, sizeof(line), "重放丢弃: %u 帧（密文层第二道闸，P1-4）  重同步: %u 次（对方重启）\r\n",
                 (unsigned)crypto_replay_drop_count(), (unsigned)crypto_replay_resync_count());
        cli_print(line);
        snprintf(line, sizeof(line), "表容量: 去重淘汰=%u 路由过期=%u/%u 地址过期=%u/%u 分片槽淘汰=%u\r\n",
                 (unsigned)mesh_dedup_evict_count(),
                 (unsigned)route_expired_count(), (unsigned)route_evict_count(),
                 (unsigned)addr_map_expired_count(), (unsigned)addr_map_evict_count(),
                 (unsigned)frag_slot_evict_count());
        cli_print(line);
        snprintf(line, sizeof(line), "ADR: 对齐=%s 信标=%u 多跳=%u 失步=%u 多锚点=%u SF=%u\r\n",
                 adr_has_beacon() ? (adr_compat_mode() ? "降级" : "是") : "否",
                 (unsigned)adr_beacon_count(), (unsigned)adr_beacon_hop_count(),
                 (unsigned)adr_desync_count(), (unsigned)adr_multi_anchor_count(),
                 (unsigned)adr_current_sf());
        cli_print(line);
        char peer_ver[16];
        uint8_t pv = mesh_peer_proto_version();
        if (pv == 0) snprintf(peer_ver, sizeof(peer_ver), "未知");
        else         snprintf(peer_ver, sizeof(peer_ver), "v%u", (unsigned)pv);
        snprintf(line, sizeof(line), "协议: 本机=%s 对端=%s 不一致=%u 次\r\n",
                 version_proto_str(), peer_ver, (unsigned)mesh_proto_mismatch_count());
        cli_print(line);
        snprintf(line, sizeof(line), "丢包: 收包队列=%u 射频请求=%u 发送队列=%u UART=%u USB=%u\r\n",
                 (unsigned)radio_rx_drop_count(), (unsigned)radio_req_drop_count(),
                 (unsigned)rtos_tx_drop_count(), (unsigned)uart_fc_drop_count(),
                 (unsigned)usb_cdc_drop_count());
        cli_print(line);
    } else if (strcmp(cmd, "stats") == 0) {
        stats_t s;
        stats_collect(&s);
        stats_print(&s);
    } else if (strcmp(cmd, "crypto") == 0) {
        if (n >= 2) {
            if (strcmp(arg1, "off") == 0) {
                crypto_disable();
                cli_print("加密已关闭\r\n");
            } else if (crypto_set_key_hex(arg1)) {
                cli_print("密钥已设置，加密开启（全网节点须同一密钥）\r\n");
            } else {
                cli_print("用法：crypto <32个hex字符> | crypto off\r\n");
            }
        } else {
            cli_print(crypto_enabled() ? "加密：开\r\n" : "加密：关\r\n");
        }
    } else if (strcmp(cmd, "wifi") == 0) {
        if (n >= 2 && strcmp(arg1, "off") == 0) {
            ota_stop();
            cli_print("已退出 WiFi 维护模式\r\n");
        } else {
            cli_print("启动 WiFi 维护模式（LoRa 暂停）…\r\n");
            ota_start();
            cli_print("AP 已开，手机连 WiFi 后浏览器开 192.168.4.1\r\n");
        }
    } else if (strcmp(cmd, "scan") == 0) {
        char line[96];
        snprintf(line, sizeof(line), "最近收包 rssi=%d snr=%d\r\n",
                 (int)radio_lastRssi(), (int)radio_lastSnr());
        cli_print(line);
        neighbor_t nb[ROUTE_MAX_NEIGHBORS];
        uint8_t n = route_neighbors(nb, ROUTE_MAX_NEIGHBORS);
        if (n == 0) {
            cli_print("邻居：无\r\n");
        } else {
            cli_print("邻居：\r\n");
            for (uint8_t i = 0; i < n; i++) {
                snprintf(line, sizeof(line), "  node_id=%u rssi=%d last_seen=%lums 前\r\n",
                         nb[i].node_id, nb[i].last_rssi,
                         (unsigned long)(millis() - nb[i].last_seen_ms));
                cli_print(line);
            }
        }
    } else if (strcmp(cmd, "ping") == 0) {
        if (n >= 2) {
            int id = atoi(arg1);
            if (id < 1 || id > 254) {
                cli_print("用法：ping <node_id 1~254>\r\n");
            } else {
                char line[96];
                snprintf(line, sizeof(line), "ping %d ...\r\n", id);
                cli_print(line);
                int16_t rssi = 0;
                int32_t rtt = mesh_ping((uint8_t)id, 3000, &rssi);
                if (rtt < 0) {
                    cli_print("  超时（无响应）\r\n");
                } else {
                    snprintf(line, sizeof(line), "  通：rtt=%ldms rssi=%ddBm\r\n",
                             (long)rtt, (int)rssi);
                    cli_print(line);
                }
            }
        } else {
            cli_print("用法：ping <node_id>\r\n");
        }
    } else if (strcmp(cmd, "cfg") == 0) {
        if (n >= 3) {
            // 拼 "key=value" 广播给全网
            char kv[64];
            snprintf(kv, sizeof(kv), "%s=%s", arg1, arg2);
            if (mesh_send_ctrl(FRAME_CFG, 0, QOS1, false, (const uint8_t*)kv, strlen(kv))) {
                cli_print("已广播配置（节点重启后生效）\r\n");
            } else {
                cli_print("发送失败\r\n");
            }
        } else {
            cli_print("用法：cfg <键> <值>（如 cfg power 15）；role 不允许远程改\r\n");
        }
    } else if (strcmp(cmd, "ver") == 0) {
        char line[64];
        snprintf(line, sizeof(line), "EMlink 固件 %s · 协议 %s\r\n",
                 version_fw_str(), version_proto_str());
        cli_print(line);
    } else {
        cli_print("未知命令，输入 help\r\n");
    }
}

static void cli_task_func(void* arg) {
    (void)arg;
    cli_banner();
    uint8_t b;
    for (;;) {
        while (usb_cli_available()) {
            usb_cli_read(&b, 1);
            char c = (char)b;
            if (c == '\r' || c == '\n') {
                if (g_line_len > 0) {
                    g_line[g_line_len] = '\0';
                    cli_print("\r\n");
                    cli_handle_line(g_line);
                    cli_print("> ");
                    g_line_len = 0;
                }
            } else if (c == '\b' || c == 0x7F) {
                if (g_line_len > 0) g_line_len--;
            } else if (g_line_len < sizeof(g_line) - 1) {
                g_line[g_line_len++] = c;
                usb_cli_write(&b, 1);   // 回显
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void cli_init() {
    g_line_len = 0;
}

void cli_start(void) {
    xTaskCreatePinnedToCore(cli_task_func, "cli", CLI_TASK_STACK,
                            nullptr, CLI_TASK_PRIO, &g_cli_task, CLI_TASK_CORE);
}
