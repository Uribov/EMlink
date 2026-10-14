/**
 * ============================================================
 * 无人机 915MHz LoRa Mesh 数传 —— 固件入口
 * 仿 PX4：rtos_comms_init() 建基础设施，再按角色依次 xxx_start()。
 * 启动顺序见 mesh_firmware_architecture.md §3.4（下游先就位，上游后跑）。
 * ============================================================
 */
#include <Arduino.h>
#include "pinmap.h"
#include "rtos.h"
#include "service/nvs_cfg.h"
#include "drivers/radio.h"
#include "drivers/uart_fc.h"
#include "drivers/usb_cdc.h"
#include "drivers/power_detect.h"
#include "link/mac.h"
#include "link/adr.h"
#include "net/mesh.h"
#include "service/mav_router.h"
#include "service/frag.h"
#include "service/addr_map.h"
#include "service/watchdog.h"
#include "service/crypto.h"
#include "app/role.h"
#include "app/cli.h"
#include "app/stats.h"
#include "app/failsafe_policy.h"

static cfg_t g_cfg;

// ---- mesh 收包回调：分片重组 / MAVLink 喂入 / addr_map 学习 ----
static void on_frag_done(const uint8_t* full, size_t len) {
    mav_router_feed(full, len);
}

static void on_mesh_rx(const frame_t* f, float rssi, float snr) {
    (void)rssi; (void)snr;
    if (f->orig_sysid != 0) addr_map_learn(f->orig_sysid, f->src_id);
    if (f->frag) {
        frag_hdr_t hdr;
        memcpy(&hdr, f->frag_hdr, 4);
        frag_reassemble(f->orig_sysid, &hdr, f->payload, f->payload_len, on_frag_done);
    } else {
        mav_router_feed(f->payload, f->payload_len);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n===== LoRa Mesh 数传 boot =====");

    // 1. 配置
    nvs_cfg_load(&g_cfg);
    crypto_init();   // 读密钥，判定加密开关（在射频/组网前，收发共用同一开关）

    // 2. 通信基础设施（最先，幂等）
    rtos_comms_init();

    // 3. 射频 + 链路 + 网络（core0，下游先就位）
    if (!radio_init(g_cfg.freq, g_cfg.bw, g_cfg.sf, g_cfg.cr, g_cfg.syncword, g_cfg.power)) {
        delay(50);
        radio_init(g_cfg.freq, g_cfg.bw, g_cfg.sf, g_cfg.cr, g_cfg.syncword, g_cfg.power);
    }
    radio_start();
    adr_init();          // ADR：先停 SF10 等 BEACON（GCS 会主动发锚点）
    mac_set_params(g_cfg.ack_timeout_ms, g_cfg.retries);
    mac_start();
    mesh_init();
    mesh_set_rx_cb(on_mesh_rx);
    mesh_start();

    // 4. MAVLink 路由（core1，被动收队列，先就位）
    role_apply(g_cfg.role);
    mav_router_start();

    // 5. 角色相关数据通路（上游，后跑，防开机首包丢）
    if (g_cfg.role == ROLE_AIR) {
        uart_fc_init(g_cfg.uart_baud);
        uart_fc_start();
    } else if (g_cfg.role == ROLE_GCS) {
        usb_cdc_init();
        usb_cdc_start();
    }

    // 6. 交互 + 统计 + 安全
    cli_init();
    cli_start();
    stats_init();
    stats_start();
    watchdog_init();
    failsafe_init();
    failsafe_enter();
    power_detect_init();

    Serial.printf("role=%d node_id=%d sysid=%d\n", g_cfg.role, g_cfg.node_id, g_cfg.sysid);
    Serial.println("===== 就绪 =====");
}

void loop() {
    // 仅配置模式（role 未设）保留串口回显，验证 USB CDC 通路；
    // 正式角色下 Serial 由 usb_task 接管做 MAVLink 数据，这里不抢。
    if (g_cfg.role == ROLE_UNSET) {
        while (Serial.available()) {
            Serial.write(Serial.read());
        }
    }
    delay(10);
}
