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
#include "net/route.h"
#include "service/mav_router.h"
#include "service/frag.h"
#include "service/addr_map.h"
#include "service/watchdog.h"
#include "service/crypto.h"
#include "service/ota.h"
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

    // NACK：交给 mav_router 补发缺失分片
    if (f->type == FRAME_NACK) {
        mav_router_on_nack(f->payload, f->payload_len);
        return;
    }

    // CFG：远程参数同步（GCS 广播 key=value）
    if (f->type == FRAME_CFG) {
        nvs_cfg_apply_kv(f->payload, f->payload_len);
        return;
    }

    if (f->orig_sysid != 0) addr_map_learn(f->orig_sysid, f->src_id);
    // sysid 撞号检测：远端节点用着与本机相同的 MAVLink sysid（多机最常见的配置错误）
    // 排除自己中继回来的帧（src_id == 本机 node_id）
    if (f->orig_sysid != 0 && f->src_id != mesh_my_id()) mav_router_note_peer_sysid(f->orig_sysid);
    if (f->frag) {
        frag_hdr_t hdr;
        memcpy(&hdr, f->frag_hdr, 4);
        frag_reassemble(f->src_id, f->orig_sysid, &hdr, f->payload, f->payload_len, on_frag_done);
    } else {
        mav_router_feed(f->payload, f->payload_len);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n===== LoRa Mesh 数传 boot =====");

    stats_led_init();    // 尽早初始化 LED：启动序列从这一刻起算（S14）
    stats_led_start();   // 独立 20ms 任务驱动，射频探测阻塞期间照样闪

    // 1. 配置
    nvs_cfg_load(&g_cfg);
    // 首次上电（NVS 里 node_id=0）：从芯片 MAC 派生并落盘，避免全网默认 1 号撞号
    if (g_cfg.node_id == 0) {
        g_cfg.node_id = nvs_cfg_assign_node_id();
        nvs_cfg_save(&g_cfg);
        Serial.printf("[cfg] node_id 自动分配 = %u（MAC 派生，已存 NVS）\n", g_cfg.node_id);
    }
    crypto_init();   // 读密钥，判定加密开关（在射频/组网前，收发共用同一开关）

    // 2. 通信基础设施（最先，幂等）
    rtos_comms_init();

    // 3. 射频 + 链路 + 网络（core0，下游先就位）
    if (!radio_init(g_cfg.freq, g_cfg.bw, g_cfg.sf, g_cfg.cr, g_cfg.syncword, g_cfg.power)) {
        Serial.println("[radio] 初始化失败：LED 红灯，CLI status 可见；rf_task 每 10s 自动重试");
    }
    radio_start();
    adr_init();          // ADR：先停 SF9 等 BEACON（GCS 会主动发锚点）
    mac_set_params(g_cfg.ack_timeout_ms, g_cfg.retries);
    mac_start();
    mesh_init();
    mesh_set_rx_cb(on_mesh_rx);
    route_init();
    addr_map_init();
    frag_init();
    mesh_start();

    // 4. MAVLink 路由（core1，被动收队列，先就位）
    role_apply(g_cfg.role);
    mav_router_start();

    // 5. 角色相关数据通路（上游，后跑，防开机首包丢）
    if (g_cfg.role == ROLE_AIR) {
        uart_fc_init(g_cfg.uart_baud);
        uart_fc_start();
    }
    // GCS/RELAY/UNSET 不再在此单独起数据通路：
    // P1-6 单口分流后，USB 通路对三种角色都开（CLI 就在这个口上）。

    // P1-6：USB 任务无条件启动——CLI 走 USB 这一个口，三种角色都要起；
    // 否则首次上电 role 未设（ROLE_UNSET）时 GCS 分支不执行，就没有命令入口。
    usb_cdc_init();
    usb_cdc_start();

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
    // P1-6：不再在此回显——Serial 的读已由 usb_task 独占（单口分流的唯一读者），
    // 这里再读会抢走本该给 CLI 的字节。通路是否正常由 CLI 自身体现（敲 help 有回显）。
    ota_boot_confirm_tick();   // P1-5：系统健康后向 bootloader 确认固件（取消回滚）
    ota_auto_tick();           // 上电 auto_wifi_s 秒且无链路活动 → 自动进 WiFi 维护模式（S5）

    // EV_CFG_CHANGED：CLI save 后置位 → 重载内存镜像并提示需 reset
    if (evg && (xEventGroupGetBits(evg) & EV_CFG_CHANGED)) {
        xEventGroupClearBits(evg, EV_CFG_CHANGED);
        nvs_cfg_load(&g_cfg);
        Serial.println("配置已保存：射频/角色类参数需 reset 重启生效");
    }

    delay(10);
}
