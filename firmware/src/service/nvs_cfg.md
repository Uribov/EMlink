# nvs_cfg —— NVS 配置

## 职责
所有配置的持久化读写（架构 §9），CLI 的 `set/get/save` 都落到这里。

## 依赖
- Arduino `Preferences`（NVS 封装）
- 被 `cli`（读写）、各模块启动（读参数）使用

## 配置项（对齐架构 §9）

| 键 | 默认 | 说明 |
|---|---|---|
| role | 未设 | air/gcs/relay |
| node_id | 自动 | 节点地址 1~254 |
| sysid | AIR=1 GCS=255 | 系统标识 |
| freq | 915.0 | MHz |
| sf/bw/cr | 7/125/5 | 扩频参数 |
| power | 20 | dBm |
| syncword | 0x12 | 私有同步字 |
| uart_baud | 115200 | 飞控波特率 |
| ttl | 5 | 最大跳数 |
| ack_timeout_ms | 300 | ACK 耐心 |
| retries | 3 | 重发次数 |
| beacon_s | 1 | 信标周期 |
| failsafe_s | 3 | 失联判定 |
| name | — | 节点名 |

## 关键实现点
1. `nvs_cfg_load` 返回 false 表示 `role` 未设 → 上层强制进「仅 USB 配置」态（架构 §8.2）
2. 保存后发 `EV_CFG_CHANGED`，各模块重载参数（`mac_set_params` 等）

## 对外接口
见 `nvs_cfg.h`。

## 验收（P0）
- 断电重启后 `get` 读到的配置与 `save` 前一致

## 当前实现架构（供人和子 agent 阅读）

### 一句话
所有配置的"仓库"——读写 ESP32 的 NVS（掉电不丢的存储），CLI 的 set/get/save 都落到这里。

### 数据结构
- `cfg_t`：内存镜像（role/node_id/sysid/freq/sf/bw/cr/power/syncword/uart_baud/ttl/ack_timeout_ms/retries/beacon_s/failsafe_s/name）
- NVS 命名空间 `"mesh"`，用 Arduino `Preferences` 封装

### 接口
- `nvs_cfg_load(cfg_t*)`：读全部；返回 false = `role` 未设（上层强制进「仅 USB 配置」态）
- `nvs_cfg_save(cfg_t*)`：写全部
- `nvs_cfg_factory(cfg_t*)`：填默认值
- `nvs_cfg_set/get(key, val)`：单项字符串读写（CLI 的 `set/get` 用），内部做字符串↔数值映射（如 "air"↔ROLE_AIR）

### 调用时机
- 上电：main.cpp `nvs_cfg_load` 读一次，分发给各模块（radio_init/mac_set_params 等）
- 运行中：CLI `set` 单项写 NVS；`save` 后发 EV_CFG_CHANGED，各模块重载
