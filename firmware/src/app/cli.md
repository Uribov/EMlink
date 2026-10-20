# cli —— USB 命令行（单口分流里的文本通道）

## 职责

插 USB 弹出的命令行，配置 / 状态 / 调试入口。

用户能改的一切参数、能查的一切状态都从这里走，它本身不做任何业务逻辑，只做解析和转发。


## 依赖

- `usb_cdc.h`（USB 单口分流：`usb_cdc` 把非 MAVLink 的字节（不以 `0xFD/0xFE` 开头）当文本喂给 CLI）
- `nvs_cfg.h`、`crypto.h`、`ota.h`、`stats.h`、`mesh.h`、`route.h`、`radio.h`、`duty.h`（status 报占空比超限计数）、`mac.h`（status 报发送分项计数，F22）、`mav_router.h`（status 报本机 sysid 与撞号次数，P1-7）

依赖面很宽，几乎每个模块都要在 `status` 里报数；但 CLI 只是读取方，不反向驱动它们。


## 命令表（对齐架构 §9）

```
role gcs|air|relay   设置角色
set <键> <值>         改配置      get <键>      读配置
set adr 0|1           开关 ADR（0=固定配置 SF、不做信标协商；默认 1）
save                  保存        reset         重启
status                看状态      stats         看统计
factory               恢复出厂默认（清 role/密钥/参数，P0-4）
crypto <32hex>|off    设/关加密密钥
led [stream on|off]   查当前灯态 / 开关键态推送（上位机同步用，默认开）
wifi [off]            开/关 WiFi 维护模式
wifiscan              扫描周围 AP（射频诊断：能收到→发射问题，收不到→射频未工作）
scan                  扫邻居 + 最近收包信号
ping <node_id>        测通断/RTT
cfg <键> <值>         广播参数给全网（GCS，role 不允许远程改）
gpiotest              强制测试 RXEN/TXEN 电平（约20秒）
mavlink monitor on    旁路观察 MAVLink（调试，预留）
help                  帮助
```

日常用的只有 `status`、`get`、`set`、`save`、`reset` 几条；`wifiscan`、`gpiotest`、`scan` 是排障用的。


## 关键实现点

1. 行缓冲 + 回车解析；参数用空白分词
2. `set` 的取值校验统一由 `nvs_cfg` 的集中校验负责（P0-4），非法键/值报「设置失败：键名未知或值超出范围」
3. SF 与带宽必须配对（P4，LLCC68 硬约束）：`set sf 10` 在 BW125 下会被拒，失败时 CLI 追加一行提示「BW125→SF≤9、BW250→≤10、BW500→≤11」
4. `save` 后发 `EV_CFG_CHANGED`，各模块重载

CLI 自己不做参数合法性判断，一律交给 `nvs_cfg`；它只负责把失败原因原样打出来。


## 数据帧诊断判读（F21 备注，不写进固件输出）

`set usb_fwd 1` + `save` + `reset` 后，AIR 角色也会把 USB 收到的 MAVLink 帧转发到 LoRa（默认 0，行为与旧版一致）。`status` 的「数据帧」行用于定位 USB→LoRa 链路的断点：

| 发送端 | 接收端 | 结论 |
|---|---|---|
| 发=0 | — | `route_msg` 没走到 `mesh_send`（角色/来源判断问题） |
| 发>0 | 收=0 | 帧没上到空口（射频/发送队列） |
| 发>0 | 收>0 投递=0 | 投递条件或解密/重放拦截 |
| 发>0 | 收>0 投递>0 | 帧已到上层，问题在 `usb_mav_write` 或 USB 通道 |

判读顺序从发送端往接收端推：先看「发」有没有涨，再看对面「收」，最后看「投递」。


## MAC 发送分项判读（F22 备注，不写进固件输出）

`status` 的「MAC」行紧跟「数据帧」行，用于定位 `mac_send()` 内部的静默丢弃点：

| 现象 | 结论 |
|---|---|
| 调用>0 但 发射错≈调用 | 射频发射一直失败 → 查 `radio_transmit` / 射频参数 / 帧长 |
| 调用>0 限流≈调用 | 令牌桶把数据帧全丢了（QOS0 份额问题） |
| 占空等待 持续增长、完成=0 | 卡在 `duty_reserve` 死等 |
| 完成≈调用 但接收端收=0 | 帧真的发出去了，问题在空口/接收侧 |
| LBT硬发 每次都在涨 | CAD 一直判忙（该模块已知问题） |

这一行回答的是「帧到底有没有从 mac 层发出去」。


## 两级收包判读（F23 备注，不写进固件输出）

`status` 的「收包」行紧跟「MAC」行：`收包: 射频=N  mesh=N  DATA=N`，其中「射频」= `radio_rx_pkt_count()`（射频层成功读到包，含 PING）、「mesh」= `mesh_rx_count()`（`frame_decode` 成功的帧）、「DATA」= `mesh_rx_data_count()`（类型为 `FRAME_DATA` 的帧）。

| 射频=N | mesh=N | DATA=N | 结论 |
|---|---|---|---|
| 不涨 | 不涨 | 不涨 | 帧没到空中 / 接收端没收到 → 发射侧问题（对端天线、功率、频点漂移） |
| 涨 | 不涨 | 不涨 | 收到了但 `frame_decode` 失败（CRC/版本）→ 编码侧问题 |
| 涨 | 涨 | 不涨 | 收到了但类型不是 FRAME_DATA → 帧头编码问题 |
| 涨 | 涨 | 涨 | 帧到了上层 → 问题在 `mav_router_feed` 的 CRC 检查或 `usb_mav_write` |

自检：发 `ping` 时接收端「射频」与「mesh」都应增长；若「射频」不涨，说明探针本身有问题。

三个计数逐级收窄，配合上面的 MAC 行就能把断点夹在某一层里。


## 对外接口

见 `cli.h`。

对外只有 `cli_start()` 和 `cli_handle_line()` 两个入口，前者建任务、后者处理一整行。


## 验收（P0）

- 敲 `get` 能打印全部配置；`set power 20` + `save` 重启后仍生效

一条就够：能读能写能持久化。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

插 USB 弹出来的命令行（USB 单口分流里的文本通道），配置/查状态/调试的入口。


### cli_task（核1 优先级8）

- 从 `usb_cdc` 的文本环形缓冲逐字符取行（P1-6 前是「读 `Serial1`」，那个假设已推翻：USB 只有一个 CDC、`Serial1` 是未初始化的 UART1）
- 行缓冲 + 回车解析，回显字符；凑满一行（`\r`/`\n`）就调 `cli_handle_line`

任务只负责收字节和回显，真正的分派在 `cli_handle_line` 里。


### 命令表（cli_handle_line 分派）

| 命令 | 动作 |
|---|---|
| `role air\|gcs\|relay` | `nvs_cfg_set("role", ...)` |
| `set <key> <val>` | `nvs_cfg_set(key, val)`（含 SF×BW 配对校验；失败时若键是 sf/bw 追加配对提示） |
| `get [key]` | `nvs_cfg_get`（无 key 打印全部） |
| `save` | 保存 NVS 后发 `EV_CFG_CHANGED` → `main.cpp` 的 `loop()` 轮询到后重载内存镜像并提示「射频/角色类参数需 reset 生效」（P1-12 接线） |
| `reset` | `esp_restart()` |
| `factory` | `nvs_cfg_factory()` + `nvs_cfg_save()` → 恢复出厂默认（P0-4） |
| `status` | 分层显示（T-3：驱动 / 中间 / 应用），纯展示层重构、数字与旧版逐项对应：① 一行上下文（角色·node_id·sysid·名字·协议版本 本机/对端）② `--- 驱动层 ---` 射频诊断（就绪=是/否；模块=在线 / 未检测到(err) / 初始化失败(err, 已试N次)；请求超时）+ 外设（收包队列丢·射频请求丢·UART丢·USB丢） ③ `--- 中间层 ---` 链路：占空比超限放行·认证失败·重放丢弃·重同步·限流丢弃(控制类/超桶)·协议不一致；网络：去重淘汰·TTL丢弃·路由过期/淘汰·地址过期/淘汰·分片槽淘汰·发送队列丢（归中间层：背压点）；MAV：USB收帧·校验失败·usb_fwd=on/off，下一行「数据帧: 发=N 收=N 投递=N」（F21 诊断），下一行「MAC: 调用=N 完成=N 发射错=N 限流=N 占空等待=N LBT硬发=N ACK成=N ACK失=N 重试=N」（F22 诊断），再下一行「收包: 射频=N mesh=N DATA=N」（F23 诊断）；速率：ADR 对齐(是/降级/否)·信标·多跳·失步·多锚点·SF·beacon_s·adr=on/off ④ `--- 应用层 ---` 流量：遥测降频·sysid撞号·node_id撞号；系统：heap·wdt复位·射频重建 |
| `ver` | 打印固件版本 + 协议版本（T-2）：`EMlink 固件 0.1.0 · 协议 v2` |
| `stats` | `stats_print` |
| `crypto <32hex>\|off` | 设密钥 / 关闭加密 / 空=看状态 |
| `wifi [off]` | 开/关 WiFi 维护模式（ota 模块） |
| `wifiscan` | 暂停 LoRa（必要时先 `ota_stop`）→ `WiFi.scanNetworks()` 扫周围 AP，打印前 10 个（SSID/rssi/ch）→ 关 WiFi、`radio_resume()`；用于区分射频整体不工作还是仅发射链路故障 |
| `scan` | 最近收包 rssi/snr + 邻居表（`route_neighbors`） |
| `ping <node_id>` | `mesh_ping` 发 FRAME_PING 测 RTT + rssi |
| `cfg <key> <val>` | `mesh_send_ctrl(FRAME_CFG)` 广播参数给全网 |
| `gpiotest` | 强制 RXEN/TXEN 电平测试（四段各 5 秒；用 `PIN_LORA_RXEN`/`PIN_LORA_TXEN`，结束后 `radio_resume()` 交回 RadioLib） |
| `help` | 帮助 |

分派就是一条长长的 if-else 链，每个分支只做一件事：拼参数、调对应模块、打印结果。


### 依赖

- `usb_cdc`（USB 单口文本通道）、`nvs_cfg`（读写）、`crypto`（密钥命令）、`ota`（wifi/wifiscan 命令）、`WiFi.h`（wifiscan 扫描）、`stats`（stats 命令）、`mesh`（ping/cfg 发帧、本机 node_id、status 认证失败计数 P1-8）、`route`（scan 邻居表）、`radio`（scan 信号）、`duty`（status 占空比计数，P1-2）、`addr_map`（status 表容量计数，P1-9）、`frag`（status 分片槽淘汰计数，P1-9）、`mav_router`（status 本机 sysid + 撞号计数，P1-7）

这里列的是实现侧实际 include 的模块，与上面「职责」段的依赖一致。


### 预留命令（架构 §9 里有、尚未实现）

`mavlink monitor`（旁路观察）——待后续补
