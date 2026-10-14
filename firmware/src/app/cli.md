# cli —— USB CDC1 命令行

## 职责
插 USB 弹出的命令行，配置 / 状态 / 调试入口。

## 依赖
- `usb_cdc.h`（CDC1 收发）、`nvs_cfg.h`、`crypto.h`、`ota.h`、`stats.h`、`mesh.h`、`route.h`、`radio.h`、`duty.h`（status 报占空比超限计数）

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
3. `save` 后发 `EV_CFG_CHANGED`，各模块重载

## 对外接口
见 `cli.h`。

## 验收（P0）
- 敲 `get` 能打印全部配置；`set power 20` + `save` 重启后仍生效

## 当前实现架构（供人和子 agent 阅读）

### 一句话
插 USB 弹出来的命令行（CDC1），配置/查状态/调试的入口。

### cli_task（核1 优先级8）
- 读 `Serial1`（CDC1），行缓冲 + 回车解析，回显字符
- 循环里逐字符读，凑满一行（`\r`/`\n`）就调 `cli_handle_line`

### 命令表（cli_handle_line 分派）
| 命令 | 动作 |
|---|---|
| `role air\|gcs\|relay` | `nvs_cfg_set("role", ...)` |
| `set <key> <val>` | `nvs_cfg_set(key, val)` |
| `get [key]` | `nvs_cfg_get`（无 key 打印全部） |
| `save` | 发 `EV_CFG_CHANGED`（各模块重载） |
| `reset` | `esp_restart()` |
| `factory` | `nvs_cfg_factory()` + `nvs_cfg_save()` → 恢复出厂默认（P0-4） |
| `status` | 四行：role / node_id+撞号冲突计数 / RF 就绪状态 / **占空比超限放行次数**（P1-2 扩充） |
| `stats` | `stats_print` |
| `crypto <32hex>|off` | 设密钥 / 关闭加密 / 空=看状态 |
| `wifi [off]` | 开/关 WiFi 维护模式（ota 模块） |
| `scan` | 最近收包 rssi/snr + 邻居表（`route_neighbors`） |
| `ping <node_id>` | `mesh_ping` 发 FRAME_PING 测 RTT + rssi |
| `cfg <key> <val>` | `mesh_send_ctrl(FRAME_CFG)` 广播参数给全网 |
| `help` | 帮助 |

### 依赖
- `usb_cdc`（CDC1 收发）、`nvs_cfg`（读写）、`crypto`（密钥命令）、`ota`（wifi 命令）、`stats`（stats 命令）、`mesh`（ping/cfg 发帧）、`route`（scan 邻居表）、`radio`（scan 信号）、`duty`（status 占空比计数，P1-2）

### 预留命令（架构 §9 里有、尚未实现）
`mavlink monitor`（旁路观察）——待后续补
