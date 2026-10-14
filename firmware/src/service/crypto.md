# crypto —— 帧载荷加密（AES-128-CCM）

## 职责
只加密帧的 **payload**，帧头不加密（要能路由/转发）。防空中监听，附完整性认证（tag）+ 防重放（nonce 含 seq/src_id）。

## 状态
**已实现**。默认关闭；密钥 16 字节存 NVS（`nvs_cfg` 的 `crypto_key`），全 0 = 关闭。

## 依赖
- `mbedtls/ccm.h`（Arduino-ESP32 框架自带，`MBEDTLS_CCM_C` 默认启用）
- `frame_codec.h`（`FRAME_TAG_LEN`/`FRAME_MAX_PLAIN`/`FLAG_ENC`）
- `nvs_cfg.h`（密钥读写）

## 算法与参数（方案定死，勿改）
| 项 | 值 | 说明 |
|---|---|---|
| 算法 | AES-128-CCM | mbedTLS，`MBEDTLS_CIPHER_ID_AES` keybits=128 |
| 密钥 | 16 字节 | NVS `crypto_key`，全 0 = 关闭 |
| tag | 4 字节 | 密文末尾，认证 + 防篡改 |
| nonce | 13 字节 | `[0..9]=0`、`[10]=src_id`、`[11..12]=seq`(小端) |
| AAD | 空 | 帧头完整性靠 CRC，首要目标是防窃听 |
| 加密范围 | 仅 payload | 帧头明文（路由/转发要读） |

## 挂接点（关键：转发不解密）
- **发送**：`mesh_send` / `mav_router` 分片路径，`frame_encode` 前 `crypto_seal_frame`（明文 → 密文+tag，置 `enc`）
- **接收**：`mesh_task` 投递本机前，若 `enc` 则 `crypto_open_frame` 解密；认证失败丢弃（计 drop_crc）
- **转发**：**原样转密文**，不解密不重加密。nonce 只依赖 `(src_id, seq)`，转发不改这俩字段，故密文可原样过中继
- BEACON/ACK 空 payload，`crypto_seal_frame` 对空 payload 返回 false → 自然不加密

## 分片与密文长度
- 加密后 payload = 明文 + 4B tag，故**明文上限降到 175**（`FRAME_MAX_PLAIN`）
- frag 分片片大小从 179 → 175，加密后每片密文 179 正好塞满 payload 区
- 影响：`mesh_send` 入参上限、`mav_router` 分片阈值、`frag_split/reassemble` 片大小，全部改用 `FRAME_MAX_PLAIN`

## 对外接口
见 `crypto.h`：`crypto_init / crypto_enabled / crypto_seal_frame / crypto_open_frame / crypto_set_key_hex / crypto_disable`。

## 验收
- 两台同密钥：A 发 → B 收，payload 无损还原
- 密钥不同 / 未设：密文帧认证失败被丢弃（不崩、不转发）
- 密文经中继转发，终点仍正确解密（转发不解密）

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_key[16]`：运行时密钥缓存（init 时从 NVS 读入）
- `g_enabled`：是否启用（密钥非全 0，`volatile`）
- `g_key_mux`：portMUX 锁，保护 `g_key`（跨核：core0 加解密 + core1 设密钥）

### 数据流
```
发送：明文 payload ──crypto_seal_frame──> 密文+tag（f->payload 指 scratch，enc=1）──frame_encode──> 空中
接收：frame_decode ──> f.enc? ──crypto_open_frame──> 明文 ──> g_rx_cb
转发：frame_decode ──> 密文原样 frame_encode（不改 payload，enc 保留）
```

### 关键实现细节
1. **nonce 防重放**：`(src_id, seq)` 唯一标识一帧；seq 每源自增回绕，src_id 区分源，nonce 不重复。转发不改 src_id/seq，故密文可原样过中继（不重加密）。
2. **输出布局**：`mbedtls_ccm_encrypt_and_tag` 输出密文与明文等长，tag 追加在密文后（`output + length`）。
3. **原地改帧**：`crypto_seal_frame` 把 `f->payload` 改指向调用方给的 `scratch`（密文+tag），`f->payload_len += 4`，`f->enc = true`。scratch 须存活到 `frame_encode` 拷贝完成。
4. **加密失败语义**：未启用 / 空 payload / 明文超 175 → 返回 false，调用方**按明文原样处理**（不丢帧）。密钥错误只发生在解密侧（认证失败 → 丢弃）。
5. **密钥管理**：`crypto_set_key_hex`（32 hex → 16B，写 NVS 立即生效）、`crypto_disable`（清 0 写 NVS）。`crypto_init` 在 main.cpp 组网前调用，收发共用同一开关。
6. **tag 常量来源**：`FRAME_TAG_LEN=4` 定义在 `frame_codec.h`（帧格式层），crypto/frag 共用，避免魔数散落。
7. **跨核锁**：seal/open 先 portMUX 拷 `g_key` 到局部再 setkey（锁只护 16B 拷贝，不护整个加解密，避免拖慢）；设密钥/关闭时 portMUX 写 `g_key`。

### 与 frame_codec 的分工
- frame_codec 只负责 flags **bit0（FLAG_ENC）** 的编解码 + `frame_t.enc` 字段，**不做加解密**
- crypto 是唯一做加解密的模块，挂在 frame_codec 的「组帧前 / 拆帧后」
