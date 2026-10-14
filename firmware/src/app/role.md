# role —— 角色逻辑

## 职责
按 NVS 里的 `role` 决定这台机器「接谁、跑哪条数据通路」。

## 依赖
- `nvs_cfg.h`（读 role）、`uart_fc`/`usb_cdc`/`mesh`/`mav_router`（初始化目标）

## 三角色数据通路
| 角色 | 数据通路 |
|---|---|
| AIR | UART(飞控) ↔ mesh ↔ LoRa；mav_router 用飞控 sysid |
| GCS | USB CDC0(QGC) ↔ mesh ↔ LoRa；mav_router 用 255 |
| RELAY | 仅 mesh 转发，不接飞控/USB 数据 |

## 关键实现点
- `role_apply` 后调 `failsafe_enter()` 进入电源/接口状态机（架构 §8.2）
- `role==ROLE_UNSET` → 强制「仅 USB 配置」态，等 CLI 设角色

## 对外接口
见 `role.h`。

## 验收（P0）
- 设 `role air` 重启 → 只开 UART+LoRa 通路；设 `role gcs` → 只开 USB+LoRa 通路
