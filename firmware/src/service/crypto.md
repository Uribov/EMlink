# crypto —— 帧载荷加密（AES-128-CCM）

## 职责
只加密帧的 **payload**，帧头不加密（要能路由/转发）。防空中监听 + 防帧头篡改（AAD）+ 防重放（nonce 含世代号/源号/序号 + 收端重放窗）。

## 状态
**已实现**（P1-4 加固后）。默认关闭；密钥 16 字节存 NVS（`nvs_cfg` 的 `crypto_key`），全 0 = 关闭。

## 依赖
- `mbedtls/ccm.h`（Arduino-ESP32 框架自带，`MBEDTLS_CCM_C` 默认启用）
- `frame_codec.h`（`FRAME_TAG_LEN`/`FRAME_MAX_PLAIN`/`FLAG_ENC`/`enc_epoch`）
- `nvs_cfg.h`（密钥读写 + `nvs_cfg_next_enc_epoch()`）

## 算法与参数
| 项 | 值 | 说明 |
|---|---|---|
| 算法 | AES-128-CCM | mbedTLS，`MBEDTLS_CIPHER_ID_AES` keybits=128 |
| 密钥 | 16 字节 | NVS `crypto_key`，全 0 = 关闭 |
| tag | 4 字节 | 密文末尾，认证 + 防篡改 |
| nonce | 13 字节 | `[0..8]=0`、`[9]=enc_epoch`、`[10]=src_id`、`[11..12]=seq`(小端) |
| AAD | 7 字节 | 逐跳不变的帧头：`src_id, dst_id, type, enc_epoch, orig_sysid, seq(2B)` |
| 加密范围 | 仅 payload | 帧头明文（路由/转发要读） |

**AAD 为什么排除 `ttl`/`relay_id`**：中继每跳都改这两个（`ttl--`、`relay_id=本机`），放进 AAD 会让正常转发的帧在收端认证失败。

## 挂接点（关键：转发不解密）
- **发送**：`mesh_send` / `mav_router` 分片路径，`frame_encode` 前 `crypto_seal_frame`（明文 → 密文+tag，置 `enc`+`enc_epoch`）
- **接收**：`mesh_task` 投递本机前，若 `enc` 则 `crypto_open_frame` 解密 → 再 `crypto_check_replay`；认证失败只跳过本机投递（P1-8：不吞转发，计 `drop_auth`）
- **转发**：**原样转密文**，不解密不重加密。nonce 只依赖 `(enc_epoch, src_id, seq)`，转发不改这三个字段，故密文可原样过中继

## 重放窗（P1-4）
- 按发送方建窗：`8` 个源 × 64 位滑动窗（bit 0 = 最新见过的 seq）
- 语义：已见（含窗口内乱序重复）→ `false`（不投递，计 `crypto_replay_drop_count`）；比窗口更旧（|差| > 63）→ `false`；前进 1~63 → 移位置位；前进 ≥64 → 窗口整体重开
- 与 mesh 去重表的分工：去重表只管 30s/256 个 seq 的重复；重放窗是**密文层**的第二道闸，管"窗口外旧密文被重新注入"

## 已知限制（诚实记录）
1. **`enc_epoch` 只有 1 字节**：同一密钥下跨 256 次开机后可能重现 nonce 复用；根治要么扩到 4 字节（协议再改一次），要么换密钥。
2. **发送方重启后可能被"饿死"**：重启后 seq 从 0 重新开始，收端窗口里该源的 `last_seq` 还在旧值上 → 新帧被判"过旧"丢弃，直到收端重启或该槽被淘汰。**上板调试（反复刷单个节点）会撞到**，见待决项 A-5c。
3. **重放窗不覆盖明文帧**：未启用加密时不受重放保护（帧头 CRC 只能挡误码，挡不了有意注入）。
4. **AAD 不含 `net_id`/`frag_hdr`/`flags` 的其余位**：篡改这些位不会触发认证失败（frag_hdr 影响重组、flags 影响路由），留待后续版本纳入。

## 分片与密文长度
- 加密后 payload = 明文 + 4B tag，故**明文上限降到 175**（`FRAME_MAX_PLAIN`）
- frag 分片片大小从 179 → 175，加密后每片密文 179 正好塞满 payload 区
- 影响：`mesh_send` 入参上限、`mav_router` 分片阈值、`frag_split/reassemble` 片大小，全部改用 `FRAME_MAX_PLAIN`

## 对外接口
见 `crypto.h`：`crypto_init / crypto_enabled / crypto_seal_frame / crypto_open_frame / crypto_set_key_hex / crypto_disable / crypto_enc_epoch / crypto_check_replay / crypto_replay_drop_count`。

## 验收
- 两台同密钥：A 发 → B 收，payload 无损还原
- 密钥不同 / 未设：密文帧认证失败被丢弃（不崩、不转发；`drop_auth` 增长，转发照旧）
- 密文经中继转发，终点仍正确解密（转发不解密）
- 用十六进制编辑器改帧头 `src_id`/`seq`/`orig_sysid` 任一字节 → 收端认证失败（AAD 生效）
- 把同一帧连续注入两次 → 第二次被重放窗拦下，`重放丢弃` 计数增长

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_key[16]`：运行时密钥缓存（init 时从 NVS 读入）
- `g_enabled`：是否启用（密钥非全 0，`volatile`）
- `g_epoch`：本次开机的世代号（`crypto_init` 里 `nvs_cfg_next_enc_epoch()` 取，**无论加密是否启用都递增**，保证单调）
- `g_key_mux`：portMUX 锁，保护 `g_key`（跨核：core0 加解密 + core1 设密钥）
- `g_replay[8]` / `g_replay_drop` / `g_replay_mux`：重放窗（core0 收帧 + core1 可能调）

### 数据流
```
发送：明文 payload ──crypto_seal_frame（盖 enc_epoch + nonce/AAD）──> 密文+tag（f->payload 指 scratch，enc=1）──frame_encode──> 空中
接收：frame_decode ──> f.enc? ──crypto_open_frame（nonce 用 f.enc_epoch）──> 明文 ──crypto_check_replay──> g_rx_cb
转发：frame_decode ──> 密文原样 frame_encode（不改 payload / enc_epoch / src_id / seq）
```

### 关键实现细节
1. **nonce 三元组**：`(enc_epoch, src_id, seq)` 唯一标识一帧；`seq` 每源自增回绕、`src_id` 区分源、`enc_epoch` 区分"同一块板子的第几次开机"。三者都在帧头上且被 AAD 绑定，中继不改，故密文可原样过中继
2. **盖世代号的时机**：`crypto_seal_frame` 里**改 payload 之前**先 `f->enc_epoch = g_epoch`——收端要照帧头重建 nonce，漏掉这一步会导致对端**全部解密失败**
3. **输出布局**：`mbedtls_ccm_encrypt_and_tag` 输出密文与明文等长，tag 追加在密文后（`output + length`）
4. **原地改帧**：`crypto_seal_frame` 把 `f->payload` 改指向调用方给的 `scratch`（密文+tag），`f->payload_len += 4`，`f->enc = true`。scratch 须存活到 `frame_encode` 拷贝完成
5. **加密失败语义**：未启用 / 空 payload / 明文超 175 → 返回 false，调用方**按明文原样处理**（不丢帧）。密钥错误只发生在解密侧（认证失败 → 只跳过投递）
6. **重放窗实现**：`bits[0]`(低 32) + `bits[1]`(高 32)，bit i = `(last_seq - i)`；`replay_shift_left()` 做 64 位整体左移（跨字进位 / n≥64 清零）；比较用 `(int16_t)(seq - last_seq)`（回绕安全）
7. **密钥管理**：`crypto_set_key_hex`（32 hex → 16B，写 NVS 立即生效）、`crypto_disable`（清 0 写 NVS）
8. **tag 常量来源**：`FRAME_TAG_LEN=4` 定义在 `frame_codec.h`（帧格式层），crypto/frag 共用，避免魔数散落
9. **跨核锁**：seal/open 先 portMUX 拷 `g_key` 到局部再 setkey（锁只护 16B 拷贝，不护整个加解密，避免拖慢）；重放窗全程 `g_replay_mux`

### 与 frame_codec 的分工
- frame_codec 只负责 flags **bit0（FLAG_ENC）** 与 `enc_epoch` 字段的编解码，**不做加解密**
- crypto 是唯一做加解密的模块，挂在 frame_codec 的「组帧前 / 拆帧后」
