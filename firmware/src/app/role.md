# role —— 角色逻辑

## 职责

按 NVS 里的 `role` 决定这台机器「接谁、跑哪条数据通路」。

本模块只决定身份和本机 sysid，不负责启动别的模块。


## 依赖

- `nvs_cfg.h`（读 role）、`uart_fc`/`usb_cdc`/`mesh`/`mav_router`（初始化目标）

依赖的是各模块的初始化入口，不反向依赖它们的运行状态。


## 三角色数据通路

| 角色 | 数据通路 |
|---|---|
| AIR | UART(飞控) ↔ mesh ↔ LoRa；mav_router 用本机 sysid（NVS 优先，默认 1） |
| GCS | USB CDC0(地面站) ↔ mesh ↔ LoRa；mav_router 用本机 sysid（NVS 优先，默认 255） |
| RELAY | 仅 mesh 转发，不接飞控/USB 数据 |

三种角色的差别只在「接哪一侧」，中间的 mesh 转发完全一样。


## 关键实现点

- `role_apply` 后调 `failsafe_enter()` 进入电源/接口状态机（架构 §8.2）
- `role==ROLE_UNSET` → 强制「仅 USB 配置」态，等 CLI 设角色
- 本机 sysid 取值：NVS `sysid` 优先，为 0 时才按角色默认（AIR=1 / GCS=255 / RELAY=0）；多机同场靠 NVS sysid 区分，撞号由 mav_router 计数（P1-7）

sysid 的取值顺序是关键：NVS 优先的规则让同场多机能各自指定，只有没设过时才退回角色默认值。


## 对外接口

见 `role.h`。

对外只有 `role_apply()` 和 `role_current()` 两个函数。


## 验收（P0）

- 设 `role air` 重启 → 只开 UART+LoRa 通路；设 `role gcs` → 只开 USB+LoRa 通路

两个方向各验一次即可。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

按 NVS 里的 `role` 决定这台机器「当谁」——AIR（接飞控）/ GCS（接地站）/ RELAY（纯中继）。


### 实现

- `role_apply(role)`：先把角色写进基础支撑（`role_state_set(role)`，D2）；本机 MAVLink sysid 按 NVS `sysid` 优先（`sysid=0` 才按角色默认：AIR=1、GCS=255、RELAY=0）→ 调 `mav_router_init(sysid)`（P1-7 前写死角色默认值，多机同场必撞号）
- `role_current()`：包装 `role_state_get()`，返回当前角色（stats 显示、LED 用）
- D2 分层：角色状态 = `service/role_state`（基础支撑，跨层共享、唯一真源），角色常量 `ROLE_*` = `service/nvs_cfg.h`；本模块只负责决定角色（读 NVS + 选 sysid）。中间层（`link/adr`、`service/ota`）改用 `role_state_get()` 读角色，不再 include `app/` —— `tools/check_deps.py` 的硬规则违规因此清零

这条分层改动是为了修掉中间层反向 include 应用层的问题，改完靠 `check_deps.py` 守住。


### 注意

实际各模块的 `xxx_start()` 启动顺序在 main.cpp 的 setup()（架构 §3.4），role 模块只负责「决定身份 + 定 sysid」，不负责启动其它模块。
