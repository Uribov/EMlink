# 模块索引 —— 大模型接入入口

> 总架构：`../../mesh_firmware_architecture.md`。本索引给大模型/开发者全局视图。

## 1. 模块清单（23 个，按层）

| 层 | 模块 | 一句话职责 |
|---|---|---|
| drivers | radio | 射频收发/CAD/复位（抽象接口，SX126x 等按宏实现） |
| drivers | airtime | LoRa 空中时间计算（Semtech 公式，纯逻辑，P1-2） |
| drivers | uart_fc | 飞控 UART（DMA 环形缓冲） |
| drivers | usb_cdc | 双 CDC：MAVLink + CLI |
| drivers | power_detect | USB 插入检测 |
| link | frame_codec | 帧编解码 + CRC（纯逻辑） |
| link | mac | LBT + ACK 重传 + 发送准入 |
| link | duty | 占空比硬上限（法规 10%） |
| link | adr | 自适应速率（BEACON 锚定固定速率表，§13.3 已实现） |
| net | mesh | 泛洪转发 + 去重 + TTL |
| net | route | 反向路径 + 路由表预留 |
| service | mav_router | MAVLink 解析 + sysid 路由 |
| service | frag | 大包分片重组 |
| service | addr_map | sysid↔node 对照表 |
| service | nvs_cfg | 配置持久化 |
| service | watchdog | 看门狗 + 射频自救 |
| service | ratelimit | 按 QoS 令牌桶流量整形（控制流保底份额，P1-10） |
| service | crypto | 帧载荷加密（AES-128-CCM，§13.1 已实现） |
| service | ota | OTA 固件升级 + WiFi Web 管理（§13.2 已实现） |
| app | role | 角色数据通路 |
| app | cli | 命令行 |
| app | stats | 统计 + LED + 喂狗 |
| app | failsafe_policy | 故障安全 + 电源状态机 |

## 2. 依赖关系（谁依赖谁）

```
app:     role ─┬─ nvs_cfg, uart_fc, usb_cdc, mesh, mav_router
         cli ─┬─ usb_cdc, nvs_cfg, crypto, ota, stats, mesh, route, radio, duty, adr, ratelimit
         stats ─┬─ (各模块计数), watchdog
         failsafe_policy ─┬─ power_detect, mav_router, nvs_cfg
service: mav_router ─┬─ frag, addr_map, mesh, crypto
         frag ─┬─ frame_codec, mesh
         addr_map (无)
         nvs_cfg (Preferences)
         watchdog ─┬─ radio
         ratelimit ─┬─ frame_codec（QOS2 常量 + frame_qos_of）
         crypto ─┬─ frame_codec, nvs_cfg
         ota ─┬─ nvs_cfg, crypto, radio, webui(页面资源)
net:     mesh ─┬─ frame_codec, route, adr, crypto, frag
         route (无)
link:    mac ─┬─ radio, frame_codec, duty, ratelimit, adr
         frame_codec (无，纯逻辑)
         duty (无，纯逻辑；空中时间由调用方传入)
         adr ─┬─ radio, mesh, role, frame_codec
drivers: radio ─┬─ RadioLib, pinmap, airtime
         airtime (无，纯逻辑；仅被 radio 使用)
         uart_fc / usb_cdc / power_detect ─┬─ pinmap
```

> 依赖只向下：drivers ← link ← net ← service ← app。`airtime` 放在 drivers 层是为了让 `radio`（drivers）能用它——若放 link 层会形成 drivers→link 的反向依赖。
> `ratelimit` 放 service 层、被 link 层的 `mac` 使用：与 `crypto`（service）被 `net/mesh` 使用同理（service 是"能力层"，可被上层按需调用）。

## 3. 建议实现顺序（自底向上，对齐架构 §11）

| 阶段 | 模块（按序） | 验收锚点 |
|---|---|---|
| **P0** | pinmap → nvs_cfg → radio → uart_fc → usb_cdc → power_detect → role → cli → stats → watchdog | 10m 互发互收 |
| **P1** | frame_codec → duty → mac | 500m 视距 0 丢包 |
| **P2** | route → mesh | 3 节点无死循环 |
| **P3** | addr_map → frag → mav_router | 2 机 + 航点 + 遥测 |
| **P4** | failsafe_policy | 断链 3s 返航 |

**扩展模块（不在 P0~P4 主线，见架构 §13）**：crypto / adr / ota 三个已全部实现（P4 后补做）。
**后补模块**：airtime（P1-2 新增，drivers 层纯逻辑工具）、ratelimit（P1-10 新增，service 层流量整形）。

## 4. 给大模型的固定提示词模板

```
读 EMlink/firmware/src/<层>/<模块>.md 和 <模块>.h，
以及 EMlink/mesh_firmware_architecture.md（相关章节见 .md 里的引用）。
按 .h 声明的接口实现 <模块>.cpp，严格按架构文档的约定，
只实现本模块，不改其他文件，不引入未声明的依赖。
```

## 5. 全局约定速查（写代码前必读）

| 主题 | 架构文档章节 |
|---|---|
| 引脚/接口/RadioLib 配置 | §0 |
| 任务/核/优先级/队列/事件 | §3（任务函数模块 static）+ `src/rtos.h`（参数/队列/事件） |
| 帧格式/地址/去重/TTL/路由 | §4 |
| MAC/信道接入（含空中时间/ACK 超时/占空比/令牌桶） | §5 + `drivers/airtime.md` + `service/ratelimit.md` |
| 分片重组 | §6 |
| sysid 路由 | §7 |
| QoS/故障安全 | §8 |
| NVS/CLI | §9 |
| 统计/日志 | §10 |
| 预留扩展点 / 已实现扩展模块 | §13（crypto/ota/adr 已实现）、§14（未来展望） |
