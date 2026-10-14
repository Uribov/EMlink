# EMlink 任务书 A-5：AEAD 加固（对应审查日志 P1-4）——nonce 加 epoch、帧头进 AAD、收端重放窗

## 背景（三个必须一起修的硬伤）

### ① nonce 必复用（当前最严重）
`service/crypto.cpp:37` 的 `build_nonce()`：nonce = `[10 个 0] + src_id + seq(16bit)`。
`mesh_next_seq()` 的 `g_seq` **每次开机从 0 开始** → 同一把密钥下，每次重启后的前若干帧 nonce 与上一次开机**完全重合**，AES-CCM 的 keystream 复用（同类明文异或即泄漏明文 + 可伪造 tag）。
修法：帧头新增 1 字节 `enc_epoch`（开机世代号，NVS 持久化，每次开机 +1），nonce 布局改为：
`[0..8]=0（9B）| [9]=enc_epoch | [10]=src_id | [11..12]=seq 小端`

### ② 无 AAD：帧头可被任意篡改
`crypto.cpp:64/95` 传的是 `nullptr, 0`（AAD 空），加密只保护 payload。
后果：攻击者（无密钥）可改帧头 `src_id`/`dst_id`/`seq`/`type`/`orig_sysid` 而不被察觉——挂到别人的源号、劫走别人的单播、把 seq 改花让收端丢帧。
修法：把**逐跳不变的帧头字段**作为 AAD。**注意 `ttl` 与 `relay_id` 必须排除**：中继每跳都会改这两个（`ttl--`、`relay_id=本机`），放进去会让正常转发的帧在收端认证失败。

### ③ 无重放防护
mesh 去重表 30s 就作废、窗口只有 256 个 seq；捕获的密文在窗口外重放会被当新帧投递。
修法：crypto 内维护**按发送方的重放窗**（64 位滑动窗，8 个源），在解密成功后校验 `seq`。

## 允许改动的文件（其它一律不动）
1. `firmware/src/link/frame_codec.h`
2. `firmware/src/link/frame_codec.cpp`
3. `firmware/src/service/crypto.h`
4. `firmware/src/service/crypto.cpp`
5. `firmware/src/service/nvs_cfg.h`
6. `firmware/src/service/nvs_cfg.cpp`
7. `firmware/src/net/mesh.cpp`（只在投递段插入重放检查）
8. `firmware/src/app/cli.cpp`（status 增加「重放丢弃」行；缺 include 则补 `crypto.h`）

**不要动**：任何 `.md`（我来写）、`main.cpp`、`stats.cpp`、`frag.cpp`、`mav_router.cpp`（调用形态不变）。

## 1. frame_codec：帧头 14 → 15 字节

`frame_codec.h` 的 `frame_t` 里，在 `net_id` 之后新增字段：
```c
    uint8_t  enc_epoch;    // 开机世代号（AES-CCM nonce 用，防跨重启 nonce 复用；未加密帧=0）
```
并把 `FRAME_HDR_LEN` 由 14 改为 15。

`frame_codec.cpp`：
- `frame_encode`：新增 `buf[10] = f->enc_epoch;`，随后 `frag_hdr` 从 `buf[11]` 开始（原 10），payload 从 `buf[15]` 开始（原 14）；CRC 覆盖 `FRAME_HDR_LEN + payload_len`（自动跟着变，不用改算法）
- `frame_decode`：对称地把 `f->enc_epoch = buf[10];`、`memcpy(f->frag_hdr, &buf[11], 4);`、`f->payload = &buf[15];`
- 更新头注释里的字节布局表（把 `enc_epoch` 插到 `net_id` 之后）
- **注意**：所有把 `14` 当帧头长度的字面量都要改成 `FRAME_HDR_LEN`

## 2. nvs_cfg：新增 `enc_epoch` 持久化计数（**不要动 `cfg_t`**）

NVS 是**逐键存取**（`Preferences` 的 putUChar/getUChar），所以世代号自己占一个键即可，**不要往 `cfg_t` 里加字段、不要改 `nvs_cfg_load/save/factory`**（老设备配置照旧可读，`getUChar("enc_epoch", 0)` 天然向后兼容）。
`nvs_cfg.h` 声明 + `nvs_cfg.cpp` 实现：
```c
// 开机调用一次：读持久化的世代号 → +1 → 写回 NVS → 返回本次开机要用的 epoch
// （NVS 只写一次/开机，无擦写磨损问题）
uint8_t nvs_cfg_next_enc_epoch(void);
```
实现要点：`Preferences p; p.begin("mesh", false)` → `uint8_t v = p.getUChar("enc_epoch", 0) + 1;` → `p.putUChar("enc_epoch", v);` → `p.end();` → 返回 `v`。
`begin` 失败也要返回递增后的值（宁可用新值，不冒 nonce 复用风险）。
注意：**不要**把 `enc_epoch` 加进 `nvs_cfg_value_ok` 白名单（它不是用户可设项）。

## 3. crypto：nonce 布局 + AAD + 重放窗

`crypto.h` 新增/修改：
```c
// 本机发送时写入帧头的开机世代号（crypto_init 内取，见下）
uint8_t crypto_enc_epoch(void);

// 收端重放检查：成功返回 true（并记账）；重放/过旧返回 false
bool crypto_check_replay(uint8_t src_id, uint16_t seq);

// 重放丢弃计数（诊断用）
uint32_t crypto_replay_drop_count(void);
```

`crypto.cpp`：
1. 新增 `static uint8_t g_epoch = 0;`；`crypto_init()` 末尾（在读完密钥之后）：
```c
    g_epoch = nvs_cfg_next_enc_epoch();
```
   ——即使加密关闭也要执行（保持世代号单调，避免"关加密跑一阵子再开"时的复用）。
2. `build_nonce` 改为：
```c
// nonce[13] 布局：[0..8]=0（9B 预留）| [9]=enc_epoch | [10]=src_id | [11..12]=seq 小端
// (enc_epoch, src_id, seq) 三元组标识一帧：跨重启不复用（P1-4）
static void build_nonce(uint8_t nonce[CRYPTO_NONCE_LEN], uint8_t epoch, uint8_t src_id, uint16_t seq) {
    memset(nonce, 0, CRYPTO_NONCE_LEN);
    nonce[9]  = epoch;
    nonce[10] = src_id;
    nonce[11] = (uint8_t)(seq & 0xFF);
    nonce[12] = (uint8_t)((seq >> 8) & 0xFF);
}
```
3. **AAD 构造**（`crypto.cpp` 内新增 static 函数）：逐跳不变字段，固定 7 字节：
```c
// AAD = 逐跳不变的帧头字段（7B）：src_id, dst_id, type, enc_epoch, orig_sysid, seq(2B)
// 排除 ttl / relay_id：中继每跳都会改（ttl--、relay_id=本机），放进去会让正常转发在收端认证失败
static size_t build_aad(const frame_t* f, uint8_t aad[7]) {
    aad[0] = f->src_id;
    aad[1] = f->dst_id;
    aad[2] = f->type;
    aad[3] = f->enc_epoch;
    aad[4] = f->orig_sysid;
    aad[5] = (uint8_t)(f->seq & 0xFF);
    aad[6] = (uint8_t)((f->seq >> 8) & 0xFF);
    return 7;
}
```
   `crypto_seal_frame`：`build_nonce(nonce, g_epoch, f->src_id, f->seq);` + `size_t aad_len = build_aad(f, aad);` 然后
   `mbedtls_ccm_encrypt_and_tag(&ctx, f->payload_len, nonce, CRYPTO_NONCE_LEN, aad, aad_len, ...)`
   `crypto_open_frame`：`build_nonce(nonce, f->enc_epoch, f->src_id, f->seq);` + 同样的 AAD 传给 `mbedtls_ccm_auth_decrypt`
   注释同步更新（删掉"帧头完整性靠 CRC"那句）。
4. 重放窗（按发送方，8 个源，每源 64 位滑动窗）：
```c
#define CRYPTO_REPLAY_SRC_NUM  8
typedef struct { uint8_t src_id; uint16_t last_seq; uint32_t bits[2]; bool in_use; } replay_slot_t;
static replay_slot_t g_replay[CRYPTO_REPLAY_SRC_NUM];
static uint32_t g_replay_drop = 0;
static portMUX_TYPE g_replay_mux = portMUX_INITIALIZER_UNLOCKED;
```
   语义（`crypto_check_replay(src_id, seq)`）：
   - 找到 `src_id` 的槽；没有就取空槽；无空槽 → 淘汰 `last_seq` 最旧（用 `(int16_t)(seq - last_seq) < 0` 判断，回绕安全）并**按"全新源"初始化**（不判定为重放，因为无从判断）
   - 空槽/新源：`last_seq = seq; bits = 0`（标记该 seq 已见）→ 返回 true
   - `d = (int16_t)(seq - last_seq)`：
     - `d == 0` 或 `d < 0`（|d| > 63 视为过旧）→ `g_replay_drop++`，返回 false
     - `d >= 64` → 整体推进：`bits` 全部左移 `d/64*64`（实现上：整字块清零/移位，可简化为"若 d>=64 则 bits[0]=bits[1], bits[1]=0；再对 d&63 做移位"），再置位
     - `0 < d < 64` → 检查 `bits` 中位 `d`；已置 → `g_replay_drop++`，返回 false；未置 → 置位并把 `last_seq` 前移 `d`（含跨字移位），返回 true
   - 全部操作在 `g_replay_mux` 临界区内（可能被 core0 mesh_task 与 core1 调用）
   - `uint32_t crypto_replay_drop_count() { return g_replay_drop; }`（只读，无锁）
   - 说明写在注释里：去重表只管 30s/256 窗口，重放窗是密文层的第二道闸

## 4. mesh.cpp：投递段插入重放检查

在投递段的解密成功分支里，**投递前**加：
```c
                if (plen == 0) { ... 保持现状 ... }
                else if (!crypto_check_replay(f.src_id, f.seq)) {
                    // 重放/过旧：不投递（转发照旧）
                    deliver = false;
                } else { ... 保持现状（rx.payload = plain; ...） ... }
```
（即：`crypto_check_replay` 返回 false 时只把 `deliver` 置 false，**不要** `continue`，不要 `g_drop_auth++`——P1-8 的"转发不受影响"必须保持。）

## 4b. cli.cpp：status 增加重放计数

在现有「认证失败」那行 `cli_print(line);` 之后追加：
```c
        snprintf(line, sizeof(line), "重放丢弃: %u 帧（密文层第二道闸，P1-4）\r\n",
                 (unsigned)crypto_replay_drop_count());
        cli_print(line);
```
（`crypto.h` 若未 include 请补；status 由七行变八行。）

## 5. 自检（请在报告里给出结论）
1. `frame_encode`/`frame_decode` 的字节偏移（10/11/15）是否互相对称、`FRAME_HDR_LEN` 是否处处用宏
2. 中继转发一个加密帧后，收端 `crypto_open_frame` 为什么仍能认证通过（用 AAD 字段集解释）
3. 同一 `(src_id, seq)` 第二次到达时 `crypto_check_replay` 的行为与计数
4. 有没有别的代码把帧头长度 `14` 写成字面量（只报告，不要改）

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检的结论
3. 你认为本次改动**不能**保证哪些性质（诚实列出）
