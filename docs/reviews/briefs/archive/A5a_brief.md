# EMlink 任务书 A-5a：帧头新增 `enc_epoch` 字段 + NVS 世代号（P1-4 第一步）

## 背景
为加密修 nonce 复用（见 A-5b），帧头要新增 1 字节"开机世代号 `enc_epoch`"，并由 NVS 持久化、每次开机 +1。
本次只做这两处**格式与配置层**改动（不动 crypto 的加解密逻辑）：

## 允许改动的文件（其它一律不动）
1. `firmware/src/link/frame_codec.h`
2. `firmware/src/link/frame_codec.cpp`
3. `firmware/src/service/nvs_cfg.h`
4. `firmware/src/service/nvs_cfg.cpp`

**不要动**：任何 `.md`、`crypto.*`、`mesh.cpp`、`cli.cpp`、`main.cpp`。

## 1. frame_codec：帧头 14 → 15 字节

`frame_codec.h`：
- `frame_t` 里在 `net_id` 之后新增字段：
```c
    uint8_t  enc_epoch;    // 开机世代号（AES-CCM nonce 用，防跨重启 nonce 复用；未加密帧=0）
```
- `FRAME_HDR_LEN` 由 14 改为 15

`frame_codec.cpp`：
- `frame_encode`：新增 `buf[10] = f->enc_epoch;`；`frag_hdr` 改为从 `buf[11]` 拷 4 字节（原 10）；`payload` 改为从 `buf[15]` 拷（原 14）。CRC 覆盖 `FRAME_HDR_LEN + payload_len`（用宏，不用改）
- `frame_decode`：对称地 `f->enc_epoch = buf[10];`、`memcpy(f->frag_hdr, &buf[11], 4);`、`f->payload = &buf[15];`
- 更新文件头注释里的字节布局表（`enc_epoch` 插在 `net_id` 之后）
- **所有**把 `14` 当帧头长度的字面量改掉（用 `FRAME_HDR_LEN`）

## 2. nvs_cfg：新增 `nvs_cfg_next_enc_epoch()`

NVS 是**逐键存取**（`Preferences` 的 putUChar/getUChar），所以世代号自己占一个键即可：
- **不要**往 `cfg_t` 里加字段、**不要**改 `nvs_cfg_load/save/factory`（老配置照旧可读，`getUChar("enc_epoch", 0)` 天然向后兼容）
- `nvs_cfg.h` 声明 + `nvs_cfg.cpp` 实现：
```c
// 开机调用一次：读持久化的世代号 → +1 → 写回 NVS → 返回本次开机要用的 epoch
// （NVS 只写一次/开机，无擦写磨损问题）
uint8_t nvs_cfg_next_enc_epoch(void);
```
实现：`Preferences p; p.begin("mesh", false)` → `uint8_t v = p.getUChar("enc_epoch", 0) + 1;` → `p.putUChar("enc_epoch", v);` → `p.end();` → 返回 `v`。
`begin` 失败也要返回递增后的值（宁可用新值，不冒 nonce 复用风险）。
**不要**把 `enc_epoch` 加进 `nvs_cfg_value_ok` 白名单（不是用户可设项）。

## 3. 自检（报告里给结论）
1. `frame_encode` / `frame_decode` 的字节偏移（10 / 11 / 15）是否互相对称
2. 全工程还有没有把 `14` 当帧头长度的字面量（只报告，不要改别的文件）
3. `nvs_cfg_load` 读老设备配置会不会因为新增键而失败（说明为什么不会）

## 完成后报告
1. 两个文件的改动摘要（原样 vs 新样子）
2. 上面 3 条自检的结论
