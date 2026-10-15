# cli —— USB 命令行（单口分流里的文本通道）

## 职责
插 USB 弹出的命令行，配置 / 状态 / 调试入口。

## 依赖
- `usb_cdc.h`（**USB 单口分流**：`usb_cdc` 把非 MAVLink 的字节（不以 `0xFD/0xFE` 开头）当文本喂给 CLI）
- `nvs_cfg.h`、`crypto.h`、`ota.h`、`stats.h`、`mesh.h`、`route.h`、`radio.h`、`duty.h`（status 报占空比超限计数）、`mav_router.h`（status 报本机 sysid 与撞号次数，P1-7）

## 命令表（对齐架构 §9）
```
role gcs|air|relay   设置角色
set <键> <值>         改配置      get <键>      读配置
save                  保存        reset         重启
status                看状态      stats         看统计
factory               恢复出厂默认（清 role/密钥/参数，P0-4）
crypto <32hex>|off    设/关加密密钥
wifi [off]            开/关 WiFi 维护模式
scan                  扫邻居 + 最近收包信号
ping <node_id>        测通断/RTT
cfg <键> <值>         广播参数给全网（GCS，role 不允许远程改）
mavlink monitor on    旁路观察 MAVLink（调试，预留）
help                  帮助
```

## 关键实现点
1. 行缓冲 + 回车解析；参数用空白分词
2. `set` 的取值校验统一由 `nvs_cfg` 的集中校验负责（P0-4），非法键/值报「设置失败：键名未知或值超出范围」
3. **SF 与带宽必须配对**（P4，LLCC68 硬约束）：`set sf 10` 在 BW125 下会被拒，失败时 CLI 追加一行提示「BW125→SF≤9、BW250→≤10、BW500→≤11」
4. `save` 后发 `EV_CFG_CHANGED`，各模块重载

## 对外接口
见 `cli.h`。

## 验收（P0）
- 敲 `get` 能打印全部配置；`set power 20` + `save` 重启后仍生效

## 当前实现架构（供人和子 agent 阅读）

### 一句话
插 USB 弹出来的命令行（**USB 单口分流里的文本通道**），配置/查状态/调试的入口。

### cli_task（核1 优先级8）
- 从 `usb_cdc` 的**文本环形缓冲**逐字符取行（P1-6 前是"读 `Serial1`"，那个假设已推翻：USB 只有一个 CDC、`Serial1` 是未初始化的 UART1）
- 行缓冲 + 回车解析，回显字符；凑满一行（`\r`/`\n`）就调 `cli_handle_line`

### 命令表（cli_handle_line 分派）
| 命令 | 动作 |
|---|---|
| `role air\|gcs\|relay` | `nvs_cfg_set("role", ...)` |
| `set <key> <val>` | `nvs_cfg_set(key, val)`（含 SF×BW 配对校验；失败时若键是 sf/bw 追加配对提示） |
| `get [key]` | `nvs_cfg_get`（无 key 打印全部） |
| `save` | 保存 NVS 后发 `EV_CFG_CHANGED` → `main.cpp` 的 `loop()` 轮询到后重载内存镜像并提示「射频/角色类参数需 reset 生效」（P1-12 接线） |
| `reset` | `esp_restart()` |
| `factory` | `nvs_cfg_factory()` + `nvs_cfg_save()` → 恢复出厂默认（P0-4） |
| `status` | **分层显示（T-3：驱动 / 中间 / 应用）**，纯展示层重构、数字与旧版逐项对应：① 一行上下文（角色·node_id·sysid·名字·协议版本 本机/对端）② `--- 驱动层 ---` 射频就绪·收包队列丢·射频请求丢·UART丢·USB丢 ③ `--- 中间层 ---` **链路**：占空比超限放行·认证失败·重放丢弃·重同步·限流丢弃(控制类/超桶)·协议不一致；**网络**：去重淘汰·TTL丢弃·路由过期/淘汰·地址过期/淘汰·分片槽淘汰·发送队列丢（归中间层：背压点）；**速率**：ADR 对齐(是/降级/否)·信标·多跳·失步·多锚点·SF·beacon_s ④ `--- 应用层 ---` **流量**：遥测降频·sysid撞号·node_id撞号；**系统**：heap·wdt复位·射频重建 |
| `ver` | 打印固件版本 + 协议版本（T-2）：`EMlink 固件 0.1.0 · 协议 v2` |
| `stats` | `stats_print` |
| `crypto <32hex>|off` | 设密钥 / 关闭加密 / 空=看状态 |
| `wifi [off]` | 开/关 WiFi 维护模式（ota 模块） |
| `scan` | 最近收包 rssi/snr + 邻居表（`route_neighbors`） |
| `ping <node_id>` | `mesh_ping` 发 FRAME_PING 测 RTT + rssi |
| `cfg <key> <val>` | `mesh_send_ctrl(FRAME_CFG)` 广播参数给全网 |
| `help` | 帮助 |

### 依赖
- `usb_cdc`（USB 单口文本通道）、`nvs_cfg`（读写）、`crypto`（密钥命令）、`ota`（wifi 命令）、`stats`（stats 命令）、`mesh`（ping/cfg 发帧、本机 node_id、status 认证失败计数 P1-8）、`route`（scan 邻居表）、`radio`（scan 信号）、`duty`（status 占空比计数，P1-2）、`addr_map`（status 表容量计数，P1-9）、`frag`（status 分片槽淘汰计数，P1-9）、`mav_router`（status 本机 sysid + 撞号计数，P1-7）

### 预留命令（架构 §9 里有、尚未实现）
`mavlink monitor`（旁路观察）——待后续补
