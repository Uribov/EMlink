#pragma once
// ============================================================
// stats —— 周期统计、LED 状态、喂狗
// 依赖：各模块统计接口；约定见架构 §10
// ============================================================
#include <Arduino.h>

// 1s 快照
struct stats_t {
    uint32_t tx_frames, rx_frames;
    uint32_t ack_ok, ack_miss, retries;
    uint32_t fwd, dropped_ttl, dropped_dup, dropped_crc;
    uint32_t reasm_timeout;
    int16_t  rssi, snr;
    uint8_t  link_quality_pct;
    uint32_t heap_free;
    uint32_t wdt_resets, rf_reinit;
};

// 初始化（启动 stats_task，1s 周期）
void stats_init();

// 采集快照（stats_task 调用；CLI 也读）
void stats_collect(stats_t* out);

// 打印统计表（CLI stats 命令用）
void stats_print(const stats_t* s);

// ---- LED（WS2818 RGB @ GPIO48）----
// 角色色（颜色 = 身份）
enum led_role_t {
    LED_ROLE_GCS   = 0,  // 绿
    LED_ROLE_AIR   = 1,  // 蓝
    LED_ROLE_RELAY = 2,  // 青（绿+蓝）
    LED_ROLE_UNSET = 3,  // 白（未配置，S12/2026-10-17 用户定）
};

// 健康节奏（节奏 = 健康）
enum led_health_t {
    LED_HEALTH_OK     = 0,  // 常亮
    LED_HEALTH_IDLE   = 1,  // 慢闪 1次/秒（无邻居）
    LED_HEALTH_LOST   = 2,  // 快闪 5次/秒（丢链）
    LED_HEALTH_CONFIG = 3,  // 双闪（配置模式）
    LED_HEALTH_FAULT  = 4,  // 红快闪（射频故障，覆盖角色色）
    LED_HEALTH_RESET  = 5,  // 红闪1下（看门狗复位）
};

// LED 当前灯态快照（供上位机同步显示；firmware 侧只读）
typedef struct {
    const char* mode;    // "boot" | "rainbow" | "fault" | "lost" | "idle" | "ok" | "off"
    uint8_t r, g, b;     // 本帧实际颜色（rainbow 模式下是当前档色）
    uint16_t half_ms;    // 闪烁半周期 ms；0 = 常亮
    uint32_t phase_ms;   // 相位（便于上位机对齐）
} led_snapshot_t;

// S14：LED 初始化与独立任务
void stats_led_init(void);    // 尽早调用（setup 开头）：只建 NeoPixel
void stats_led_start(void);   // 启动 20ms LED 任务；之后灯不依赖 loop
void stats_led_tick(void);    // 保留（任务内部调用；状态机本体）

// 取当前灯态（任意时刻可调；未初始化 LED 返回 mode="off"）
void stats_led_snapshot(led_snapshot_t* out);

// 开关灯态推送（默认开）。关掉时不再往 USB 打 #led 行。
void stats_led_stream_set(bool on);
bool stats_led_stream_get(void);

// 启动统计任务（内部 xTaskCreate stats_task_func，任务函数为模块内 static）
void stats_start(void);
