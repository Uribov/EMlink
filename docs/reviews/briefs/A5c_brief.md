# EMlink 任务书 A-5c：重放窗按 `enc_epoch` 重同步（用户已拍板：方案 A）

## 背景（为什么改）
A-5b 加了收端重放窗后有个**重启饿死**口子：

- 发送方**重启**后 `seq` 从 0 重新开始（`mesh_next_seq` 的 `g_seq` 是内存变量）
- 收端窗口里该源的 `last_seq` 还停在旧值（比如 5000）
- 新帧 `d = (int16_t)(seq - last_seq)` = 大负数，且 `|d| > 63` → 被判"过旧"**一直丢弃**
- 结果：**那台机器发的东西对端再也收不到**，直到对端也重启、或该槽被别的源挤掉

上板调试（反复刷单个节点）几乎必撞，表现为"刚刷完的板子发不出去"。

## 修法（方案 A）
`enc_epoch` 是**被 AAD 绑定**的字段（无密钥者改不动它，改了会认证失败），所以它是"对方是否重启过"的可信信号：
**同一源、`enc_epoch` 变了 → 判定对方重启过 → 重置该源的窗口并接受**；epoch 相同 → 仍走严格窗口。

**已知取舍（用户已知情并选择本方案）**：持有**旧世代**真实密文的人，可注入一帧陈旧但合法的帧（触发一次窗口重置）。30 秒内的重复仍被 mesh 去重表拦下；不选方案 B（窗口外一律丢）是因为台上 bring-up 撞到"饿死"的概率接近 100%。

## 允许改动的文件（其它一律不动）
1. `firmware/src/service/crypto.h`
2. `firmware/src/service/crypto.cpp`
3. `firmware/src/net/mesh.cpp`（调用处补参数）
4. `firmware/src/app/cli.cpp`（status 那行加"重同步"计数）

**不要动**：任何 `.md`、`main.cpp`、`frame_codec.*`、`nvs_cfg.*`、`frag.*`、`mav_router.*`。

## 1. crypto.h
- `crypto_check_replay` 签名改为三元：
```c
// 收端重放检查：成功返回 true（并记账）；重放/过旧返回 false
// epoch = 帧头 enc_epoch（被 AAD 绑定）：同源 epoch 变化 ⇒ 对方重启过 ⇒ 重置该源窗口
bool crypto_check_replay(uint8_t src_id, uint16_t seq, uint8_t epoch);
```
- 新增：
```c
// 因"对方重启（epoch 变化）"重置窗口的次数（诊断用）
uint32_t crypto_replay_resync_count(void);
```

## 2. crypto.cpp
- `replay_slot_t` 增加 `uint8_t epoch;`
- 新增 `static uint32_t g_replay_resync = 0;`
- 在 `crypto_check_replay()` 里，**取到槽之后、做序号判断之前**插入 epoch 判定：
```c
    if (!s->in_use || s->epoch != epoch) {
        // 全新源，或对方重启过（epoch 变）→ 窗口重开，按"本世代第一帧"记账
        if (s->in_use) g_replay_resync++;      // 仅"重启重同步"计数，新源不计
        s->in_use   = true;
        s->src_id   = src_id;
        s->epoch    = epoch;
        s->last_seq = seq;
        s->bits[0]  = 1u;
        s->bits[1]  = 0;
        portEXIT_CRITICAL(&g_replay_mux);
        return true;
    }
```
  （原来的 `if (!s->in_use)` 分支由上面这条统一承担，删掉旧的那个分支，别留两处。）
- 新增 `uint32_t crypto_replay_resync_count() { return g_replay_resync; }`（只读，无锁）
- 注释里写清：**为什么 epoch 可信**（AAD 绑定，伪造即认证失败）与**残余风险**（旧世代真密文可触发一次重置）

## 3. mesh.cpp
调用处补第三个参数（`f.enc_epoch`）：
```c
                else if (!crypto_check_replay(f.src_id, f.seq, f.enc_epoch)) {
```
其余逻辑（只 `deliver = false`，不 `continue`、不 `g_drop_auth++`）保持不变。

## 4. cli.cpp
把现有「重放丢弃」那行改成同时报重同步次数（**不要新增行**，status 仍是八行）：
```c
        snprintf(line, sizeof(line), "重放丢弃: %u 帧（密文层第二道闸，P1-4）  重同步: %u 次（对方重启）\r\n",
                 (unsigned)crypto_replay_drop_count(), (unsigned)crypto_replay_resync_count());
        cli_print(line);
```

## 5. 自检（报告里给结论）
1. 发送方重启后，收端第一帧为什么会被接受？（用 epoch 与 AAD 解释）
2. 同一次开机内，窗口外旧序号还会不会被接受？（说明严格窗口没有被削弱）
3. `s->epoch != epoch` 会不会把"新源"和"重启重同步"混淆？计数器为什么只统计后者？
4. 有没有别的地方调用 `crypto_check_replay` 需要一起改（全工程搜一遍，只报告）

## 完成后报告
1. 各文件改动摘要（原样 vs 新样子）
2. 上面 4 条自检的结论
3. 本次改动**不能**保证哪些性质
