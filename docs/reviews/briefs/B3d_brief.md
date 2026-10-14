# EMlink 任务书 B-3d：MAVLink 流限速（对应审查日志 P1-10 的"按消息 ID 降频"）

## 背景（为什么改）
QGC 默认会请求 10~50Hz 遥测流。而在 10% 占空比下，空口一秒最多也就发一两帧满帧（200B@SF7 = 318ms），**高频遥测根本装不下**，还会把那 90% 的用户份额吃满（令牌桶 B-3a 只管总量与优先级，**看不懂 MAVLink 语义**）。审查日志要求"按消息 ID 的白名单/降频"。

修法：在 **`to_lora` 方向**（转 LoRa 的那条路径）做**按消息类型的降频**：
- **保命/遥控类永远放行**：HEARTBEAT、COMMAND_*、SET_MODE、PARAM_*、MISSION_*、RC_CHANNELS_OVERRIDE、MANUAL_CONTROL、TIMESYNC …
- **高频遥测按最小间隔降频**（同一 msgid 两次转发间隔不足就丢）
- **表里没有的 msgid 一律放行**（不认识的不擅自丢，交给 ratelimit 管总量）

## 允许改动的文件（其它一律不动）
1. `firmware/src/service/mav_router.cpp`
2. `firmware/src/service/mav_router.h`（只加一个诊断接口声明）
3. `firmware/src/app/cli.cpp`（把降频计数并进现有那行，不新增行）

**不要动**：任何 `.md`、`mesh.*`、`ratelimit.*`、`mac.*`、`route.*`、`main.cpp`；不要跑 pio/编译；不要 git commit/add。

## 1. mav_router.cpp：限速表 + 判定函数
放在 `route_msg()` **之前**（文件中部，紧邻其它 static 状态）：
```c
// ---- P1-10：MAVLink 流限速（只作用于"转 LoRa"方向；本地串口/USB 方向不限速）----
// 为什么只能在这一层做：令牌桶（ratelimit）管总量与优先级，不理解 MAVLink 语义；
// 高频遥测（ATTITUDE/GPS/IMU…）必须按类型降频，否则用户份额会被它们吃满。
typedef struct {
    uint32_t msgid;             // MAVLink 消息 ID
    uint32_t min_interval_ms;   // 转 LoRa 的最小间隔（0 = 不限速）
    uint32_t last_ms;           // 上次放行时刻（运行时状态）
} mav_stream_rate_t;

static mav_stream_rate_t g_stream[] = {
    {  0,     0, 0 },   // HEARTBEAT：保命，不限速
    {  1,  1000, 0 },   // SYS_STATUS
    { 24,   500, 0 },   // GPS_RAW_INT
    { 26,   200, 0 },   // SCALED_IMU
    { 30,   500, 0 },   // ATTITUDE
    { 33,   500, 0 },   // GLOBAL_POSITION_INT
    { 65,   200, 0 },   // RC_CHANNELS
    { 74,   500, 0 },   // VFR_HUD
};
#define MAV_STREAM_N (sizeof(g_stream) / sizeof(g_stream[0]))

static uint32_t g_stream_drop = 0;

// 返回 false = 本帧因流限速丢弃（不转 LoRa）
static bool mav_stream_allow(uint32_t msgid, uint32_t now) {
    for (size_t i = 0; i < MAV_STREAM_N; i++) {
        if (g_stream[i].msgid != msgid) continue;
        if (g_stream[i].min_interval_ms == 0) return true;                  // 白名单：不限速
        if (now - g_stream[i].last_ms < g_stream[i].min_interval_ms) {
            g_stream_drop++;
            return false;
        }
        g_stream[i].last_ms = now;
        return true;
    }
    return true;   // 表里没有的 msgid：不擅自丢，交给 ratelimit 管总量
}
```

## 2. mav_router.cpp：挂到转 LoRa 的路径上
在 `route_msg()` 里 `if (to_lora) {` 之后、**分片/发送之前**插入一行：
```c
        if (!mav_stream_allow(msgid, millis())) return;   // P1-10：高频遥测降频（本帧不转 LoRa）
```
（`msgid` 是 `route_msg` 里已有的变量，直接用，不要重新解析。）

## 3. mav_router.h：诊断接口
```c
// 被流限速丢掉的遥测帧数（P1-10；只统计"转 LoRa"方向）
uint32_t mav_router_stream_drop_count(void);
```
`mav_router.cpp` 末尾实现：`uint32_t mav_router_stream_drop_count(void) { return g_stream_drop; }`（只读，无锁）。

## 4. cli.cpp：把降频计数并进现有那行（**不新增行**）
把 B-3a 改过的那行格式串末尾追加 `  遥测降频=%u`，参数追加 `(unsigned)mav_router_stream_drop_count()`。改完大致是：
```c
        snprintf(line, sizeof(line), "占空比: 超限放行=%u 次  限流丢弃=%u 次（控制类 %u，超桶放行 %u）  遥测降频=%u\r\n",
                 (unsigned)duty_oversize_count(), (unsigned)ratelimit_drop_count(),
                 (unsigned)ratelimit_ctrl_drop_count(), (unsigned)ratelimit_oversize_count(),
                 (unsigned)mav_router_stream_drop_count());
```
（status 仍是十一行；`mav_router.h` 已 include 则无需再加。）

## 5. 自检（报告里给结论）
1. 流限速为什么必须做在 `mav_router` 而不是 `ratelimit` 里？（提示：ratelimit 看不懂 MAVLink 语义）
2. 为什么 HEARTBEAT / COMMAND / PARAM / MISSION 这类必须永远放行？
3. 表里没有的 msgid 为什么放行而不是丢掉？
4. `last_ms` 这些 static 状态为什么不需要加锁？（说明 `route_msg` 的调用上下文）

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检结论
3. 本次改动**不能**保证哪些性质（诚实列出，例如"降频阈值没实测""没区分飞控上行与地面站下行"）
