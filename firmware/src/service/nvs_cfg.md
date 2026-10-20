# nvs_cfg —— NVS 配置

## 职责
所有配置的持久化读写（架构 §9），CLI 的 `set/get/save` 都落到这里。

## 依赖
- Arduino `Preferences`（NVS 封装）
- 被 `cli`（读写）、各模块启动（读参数）、`crypto`（世代号）使用

## 配置项（对齐架构 §9）

| 键 | 默认 | 说明 |
|---|---|---|
| role | 未设 | air/gcs/relay |
| node_id | 自动 | 节点地址 1~254 |
| sysid | AIR=1 GCS=255 | 系统标识（P1-7：NVS 优先，0=按角色默认） |
| freq | 915.0 | MHz |
| sf/bw/cr | 7/125/5 | 扩频参数。**SF 与带宽必须配对**：BW125→SF≤9、BW250→≤10、BW500→≤11（LLCC68 硬约束，见下 §6） |
| power | 20 | dBm |
| syncword | 0x12 | 私有同步字 |
| uart_baud | 115200 | 飞控波特率 |
| ttl | 5 | 最大跳数 |
| ack_timeout_ms | 300 | ACK 耐心（**下限**：实际超时 = max(它, ACK 帧 ToA+50ms)，P1-2） |
| retries | 3 | 重发次数 |
| beacon_s | 1 | 信标周期（**当前未接线**，ADR 固定 5s 锚点，见 ota/frag 同批清理项） |
| failsafe_s | 3 | 失联判定 |
| auto_wifi_s | 30 | 上电后多少秒自动进 WiFi 维护模式。**钳制**：<15→15、>60→60；**不能关闭**（没有电脑/USB 时这是唯一进门方式，S5/2026-10-17） |
| usb_fwd | 0 | AIR 角色是否把 USB 收到的 MAVLink 帧转发到 LoRa（0=否，1=是；调试开关） |
| name | — | 节点名 |
| crypto_key | 全 0 | AES-128 密钥 16B（全 0 = 加密关闭，crypto 模块用） |
| wifi_ssid | 空 | WiFi AP 名（空 = EMlink-<node_id>） |
| wifi_pass | 空 | WiFi AP 密码（空 = 无密码） |
| ota_url | 空 | 预留（将来从 URL 拉固件） |
| enc_epoch | 0（自动 +1） | **非用户可设**：开机世代号，crypto 防跨重启 nonce 复用用（P1-4） |

## 关键实现点
1. `nvs_cfg_load` 返回 false 表示 `role` 未设 → 上层强制进「仅 USB 配置」态（架构 §8.2）
2. 保存后发 `EV_CFG_CHANGED`，各模块重载参数（`mac_set_params` 等）
3. **集中校验**（P0-4）：`nvs_cfg_value_ok(key,val)` 是唯一校验入口（CLI / 远程 FRAME_CFG / 网页共用）；`nvs_cfg_set()` 写入前先过它，非法值一律拒绝——防止 `freq=0` / `sf=99` / `power=30` 这类"写进去就失联"的配置。范围：role=air/gcs/relay、node_id 1–254、freq 862–930（取 `radios/*.h` 的硬件能力宏）、sf 7–11、bw {125,250,500}、cr 5–8、power -9–22、ttl 1–10、ack_timeout_ms 50–10000、retries 0–10、beacon_s / failsafe_s 1–60、**auto_wifi_s 是例外：`value_ok` 只要求"可解析为整数"，范围由 `nvs_cfg_set` 钳制到 15–60**（用户 2026-10-17 明确"小于 15 强制 15、超过 60 强制 60"）、**usb_fwd 同样只要求整数，非 0/1 一律钳到 0**、name ≤15、uart_baud 白名单；字符串→数字用 strtol/strtod 并校验尾部（`12abc` 判非法）
4. **node_id 自动分配**（P0-4）：NVS 里为 0（未分配）时，`nvs_cfg_assign_node_id()` 从芯片 MAC 派生 1–254，`main.cpp` 首次上电落盘——避免全网默认 1 号撞号
5. **逐键存取**：`Preferences` 按 key 存，不是整块 blob —— 新增键（如 `enc_epoch`）对老设备天然向后兼容（`getUChar(key, 默认值)`）
6. **SF 与带宽必须配对**（本板芯片 LLCC68 硬约束，2026-10-15 加）：`nvs_cfg_sf_max_for_bw(bw)` 给出该带宽允许的最大 SF（125→9 / 250→10 / 500→11，其余→0），`nvs_cfg_pair_ok(sf,bw)` = `7 ≤ sf ≤ 上限`。**三处咽喉全拦**：① `nvs_cfg_set()`（CLI 单键写：读另一键的当前值一起判定）② `nvs_cfg_save()`（整表写：网页 `/set`、恢复出厂、远程 FRAME_CFG）③ `nvs_cfg_load()` 对**老配置**兜底 —— 非法组合只把内存里的 SF 降档 + `Serial` 告警，**不写回 NVS**（避免每次开机都擦写）。网页与 CLI 都给了明确报错文案。
   - **注意（已知限制）**：单键路径不能原子地同时改 sf+bw。要从 `7/125` 换到 `10/250`，得**先** `set bw 250`、**再** `set sf 10`（反过来会被当前带宽拦下）；整表写（网页 `/set`）没有这个限制。
   - 依据：`docs/references/LLCC68_E220-900M22S.md` §2（亿佰特规格书 + Semtech 手册 + Zephyr binding 三处一致）

## 对外接口
见 `nvs_cfg.h`。

## 验收（P0）
- 断电重启后 `get` 读到的配置与 `save` 前一致

## 当前实现架构（供人和子 agent 阅读）

### 一句话
所有配置的"仓库"——读写 ESP32 的 NVS（掉电不丢的存储），CLI 的 set/get/save 都落到这里。

### 数据结构
- `cfg_t`：内存镜像（role/node_id/sysid/freq/sf/bw/cr/power/syncword/uart_baud/ttl/ack_timeout_ms/retries/beacon_s/failsafe_s/auto_wifi_s/adr_en/usb_fwd/name/crypto_key[16]/wifi_ssid[32]/wifi_pass[32]/ota_url[96]）
- NVS 命名空间 `"mesh"`，用 Arduino `Preferences` 封装

### 接口
- `nvs_cfg_load(cfg_t*)`：读全部；返回 false = `role` 未设（上层强制进「仅 USB 配置」态）
- `nvs_cfg_save(cfg_t*)`：写全部
- `nvs_cfg_factory(cfg_t*)`：填默认值
- `nvs_cfg_set/get(key, val)`：单项字符串读写（CLI 的 `set/get` 用），内部做字符串↔数值映射（如 "air"↔ROLE_AIR）；`set` 写入前先过集中校验（P0-4）
- `nvs_cfg_value_ok(key, val)`（P0-4）：唯一校验入口（CLI / 远程 / 网页共用）
- `nvs_cfg_assign_node_id()`（P0-4）：MAC 派生 1–254 节点号（首次上电用）
- `nvs_cfg_next_enc_epoch()`（P1-4）：开机读 `enc_epoch` 键 → +1 → 写回 → 返回；**不进白名单**（不是用户可设项），`cfg_t` 里没有这个字段（保持逐键独立）
- `nvs_cfg_apply_kv(data, len)`：应用 "key=value"（FRAME_CFG 远程参数同步用）；白名单拒 `role`

### 调用时机
- 上电：main.cpp `nvs_cfg_load` 读一次，分发给各模块（radio_init/mac_set_params 等）；`crypto_init` 里调 `nvs_cfg_next_enc_epoch()`
- 运行中：CLI `set` 单项写 NVS；`save` 后发 EV_CFG_CHANGED，各模块重载
