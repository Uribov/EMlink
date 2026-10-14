// ============================================================
// cli.cpp —— USB CDC1 命令行
// 依赖 usb_cdc.h / nvs_cfg.h / stats.h；命令约定见架构 §9
// ============================================================
#include "cli.h"
#include "../rtos.h"
#include "../drivers/usb_cdc.h"
#include "../service/nvs_cfg.h"
#include "../app/stats.h"
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
              "  status               看状态\r\n"
              "  stats                看统计\r\n"
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
            cli_print("用法：set <键> <值>；键名见 get\r\n");
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
    } else if (strcmp(cmd, "status") == 0) {
        char val[64];
        nvs_cfg_get("role", val, sizeof(val));
        char line[96];
        snprintf(line, sizeof(line), "role=%s\r\n", val);
        cli_print(line);
    } else if (strcmp(cmd, "stats") == 0) {
        stats_t s;
        stats_collect(&s);
        stats_print(&s);
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
