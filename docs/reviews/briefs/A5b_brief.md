# EMlink 任务书 A-5b：AEAD 加固——nonce 加 epoch、帧头进 AAD、收端重放窗（P1-4 第二步）

## 前置（A-5a 已完成）
帧头已从 14 增到 15 字节，`frame_t` 已有 `enc_epoch` 字段（帧头偏移 10）；`nvs_cfg_next_enc_epoch()` 已可用。

## 背景（本次修的三个硬伤）

### ① nonce 必复用（最严重）
`crypto.cpp:37` 的 `build_nonce()`：nonce = `[10 个 0] + src_id + seq(16bit)`。
`mesh_next_seq()` 的 `g_seq` **每次开机从 0 开始** → 同一把密钥下，每次重启后的前若干帧 nonce 与上一次开机**完全重合** → AES-CCM keystream 复用（明文可被异或还原、tag 可伪造）。
修法：nonce 布局改为 `[0..8]=0（9B）| [9]=enc_epoch | [10]=src_id | [11..12]=seq 小端`。

### ② 无 AAD：帧头可被任意篡改
`crypto.cpp:64/95` AAD 传的是 `nullptr, 0`。攻击者（无密钥）可改帧头 `src_id`/`dst_id`/`seq`/`type`/`orig_sysid` 而不被察觉。
修法：把**逐跳不变的帧头字段**作为 AAD。**`ttl` 与 `relay_id` 必须排除**：中继每跳都改这两个（`ttl--`、`relay_id=本机`），放进去会让正常转发的帧在收端认证失败。

### ③ 无重放防护
mesh 去重表 30s 作废、窗口只有 256 个 seq；捕获的密文在窗口外重放会被当新帧投递。
修法：crypto 内维护**按发送方的重放窗**（64 位滑动窗，8 个源），解密成功后校验 `seq`。

## 允许改动的文件（其它一律不动）
1. `firmware/src/service/crypto.h`
2. `firmware/src/service/crypto.cpp`
3. `firmware/src/net/mesh.cpp`（只在投递段插入重放检查）
4. `firmware/src/app/cli.cpp`（status 增加「重放丢弃」行；缺 include 则补 `crypto.h`）

**不要动**：任何 `.md`、`main.cpp`、`stats.cpp`、`frag.cpp`、`mav_router.cpp`、`frame_codec.*`、`nvs_cfg.*`。

## 1. crypto.h 新增
```c
// 本机发送时写入帧头的开机世代号（crypto_init 内取，见下）
uint8_t crypto_enc_epoch(void);

// 收端重放检查：成功返回 true（并记账）；重放/过旧返回 false
bool crypto_check_replay(uint8_t src_id, uint16_t seq);

// 重放丢弃计数（诊断用）
uint32_t crypto_replay_drop_count(void);
```

## 2. crypto.cpp
### (a) `g_epoch`
新增 `static uint8_t g_epoch = 0;`；`crypto_init()` 末尾（读完密钥之后）加：
```c
    g_epoch = nvs_cfg_next_enc_epoch();   // 每次开机 +1，保证跨重启不复用
```
**即使加密关闭也要执行**（保持世代号单调）。
新增 `uint8_t crypto_enc_epoch(void) { return g_epoch; }`。

### (b) nonce
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

### (c) AAD（固定 7 字节）
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

### (d) `crypto_seal_frame`（**注意这一条最要紧**）
1. 开头（改 payload 之前）先给帧盖上世代号：**`f->enc_epoch = g_epoch;`**
   —— 收端要靠帧头里的 `enc_epoch` 重建同一个 nonce，忘了这一步会导致对端**全部解密失败**。
2. `build_nonce(nonce, g_epoch, f->src_id, f->seq);`
3. `size_t aad_len = build_aad(f, aad);` 把 `aad, aad_len` 传给 `mbedtls_ccm_encrypt_and_tag`（替换原来的 `nullptr, 0`）
4. 注释同步更新（删掉"帧头完整性靠 CRC，防窃听是首要目标"那句）

### (e) `crypto_open_frame`
1. `build_nonce(nonce, f->enc_epoch, f->src_id, f->seq);`
2. 同样的 `build_aad(f, aad)` 传给 `mbedtls_ccm_auth_decrypt`
（`f->enc_epoch` 由 frame_decode 从帧头读出）

### (f) 重放窗
```c
#define CRYPTO_REPLAY_SRC_NUM  8
typedef struct { uint8_t src_id; uint16_t last_seq; uint32_t bits[2]; bool in_use; } replay_slot_t;
static replay_slot_t g_replay[CRYPTO_REPLAY_SRC_NUM];
static uint32_t g_replay_drop = 0;
static portMUX_TYPE g_replay_mux = portMUX_INITIALIZER_UNLOCKED;
```
`crypto_check_replay(src_id, seq)` 语义：
- 找 `src_id` 的槽；没有 → 取空槽；无空槽 → 淘汰 `last_seq` 最旧者（用 `(int16_t)(seq - last_seq) < 0` 比较，回绕安全）并**按"全新源"初始化**（无从判断重放）
- 空槽/新源：`last_seq = seq;` 全 `bits = 0` 后把最低位置 1（标记该 seq 已见）→ 返回 true
- `d = (int16_t)(seq - last_seq)`：
  - `d == 0` 或 `d < 0`（且 `|d| > 63`）→ `g_replay_drop++`，返回 false
  - `d >= 64` → 整体推进：`bits[0] = bits[1]; bits[1] = 0;` 再做 `d & 63` 位的移位，然后置位 `last_seq = seq`
  - `0 < d < 64` → 检查该位；已置 → `g_replay_drop++` 返回 false；未置 → 置位并把 `last_seq` 前移 `d`（含跨字移位），返回 true
- 全程在 `g_replay_mux` 临界区内（可能被 core0 mesh_task 与 core1 调用）
- `uint32_t crypto_replay_drop_count() { return g_replay_drop; }`（只读，无锁）
- 注释说明：去重表只管 30s/256 窗口，重放窗是密文层的第二道闸

## 3. mesh.cpp：投递段插入重放检查
在投递段的解密**成功**分支里、投递之前加：
```c
                if (plen == 0) { /* 保持现状：g_drop_auth++ / deliver=false */ }
                else if (!crypto_check_replay(f.src_id, f.seq)) {
                    // 重放/过旧：不投递（转发照旧）
                    deliver = false;
                } else { /* 保持现状：rx.payload = plain; ... */ }
```
**`crypto_check_replay` 返回 false 时只把 `deliver` 置 false**：不要 `continue`、不要 `g_drop_auth++`（P1-8 的"转发不受影响"必须保持）。

## 4. cli.cpp：status 增加重放计数
在现有「认证失败」那行 `cli_print(line);` 之后追加：
```c
        snprintf(line, sizeof(line), "重放丢弃: %u 帧（密文层第二道闸，P1-4）\r\n",
                 (unsigned)crypto_replay_drop_count());
        cli_print(line);
```
（`crypto.h` 若未 include 请补；status 由七行变八行。）

## 5. 自检（报告里给结论）
1. 中继转发一个加密帧后，收端 `crypto_open_frame` 为什么仍能认证通过（用 AAD 字段集解释）
2. 同一 `(src_id, seq)` 第二次到达时 `crypto_check_replay` 的行为与计数
3. 发送方忘记 `f->enc_epoch = g_epoch` 会怎样（一句话）
4. `crypto_seal_frame` 里 `f->enc_epoch` 的赋值放在改 payload 之前还是之后有没有区别

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检的结论
3. 本次改动**不能**保证哪些性质（诚实列出，例如"epoch 只有 1 字节，256 次开机后可能重复"）
