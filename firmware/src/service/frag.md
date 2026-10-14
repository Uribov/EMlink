# frag —— 分片与重组

## 职责
MAVLink 大消息（>175B）拆成小片、对方拼回；缺件只补缺件。遥测/心跳/单航点（≤38B）**不分片**。

## 依赖
- `frame_codec.h`（`FRAME_MAX_PLAIN=175`、`FLAG_FRAG`、`frag_hdr[4]`）

## 分片（发送侧）
- `len > FRAME_MAX_PLAIN` 时拆；分片头写 `frag_hdr[4]`（msg_id/index/total/seq）
- 每片 payload ≤175B，片序 `frag_index` 0..total-1，组标识 `frag_seq` 自增

## 重组（接收侧）
- 键 = `(orig_sysid, frag_seq)`，按 `frag_index` 存到重组缓冲
- 全片到齐 → 拼接回调 `done(full, len)`
- 缺件：回 NACK（携带缺失位图），发方**只补缺的那几片**，不重发全部
- 2s 未拼齐 → 整组作废，计 `frag_timeout_count`

## 片大小 175 的由来（加密预留）
加密（crypto 模块）会给每片 payload 加 4B tag，故片大小从 179 降到 175（`FRAME_MAX_PLAIN`）：
加密后每片密文 = 175+4 = 179，正好塞满 payload 区。未加密时也按 175 切（逻辑统一，少 4B 空间换一份代码）。

## 内存注意
重组缓冲预估几 KB（4 并发 × 267B），放 SRAM 即可，不放栈；PSRAM 有余量也可用。

## 对外接口
见 `frag.h`。

## 验收（P3）
- 单条 267B MAVLink 2 消息（无签名最大帧）→ 拆 2 片 → 对端无损重组
- 人为丢 1 片 → NACK → 发方只补该片 → 重组成功

## 当前实现架构（已实现，供子 agent 分析）

### 分片头 frag_hdr_t（4B，帧偏移 10~13）
`msg_id`（占位 0）+ `frag_index`（第几片）+ `frag_total`（总片数）+ `frag_seq`（组序号自增）

### 发送侧 frag_split
- len ≤ 175 单片（返回 1）；>175 拆 ceil(len/175) 片
- hdr 输出公共部分（frag_seq 自增 / frag_total），frag_index 由调用者每片填

### 接收侧 frag_reassemble（键 =(orig_sysid, frag_seq)）
- 重组槽 `reasm_slot_t`：4 并发（FRAG_CONCURRENT），每槽 8 片 × 175B
- 槽匹配同时比 orig_sysid + frag_seq 两个字段（多机并发不串包）
- 位图 got 判拼齐；拼齐回调 `done(full, len)`
- 2s 超时懒清理（FRAG_REASM_TIMEOUT_MS），计 `frag_timeout_count`

### 调用链
- 发送：mav_router 的 route_msg 里 len>FRAME_MAX_PLAIN 时 frag_split + 逐片 crypto_seal_frame + frame_encode 投 q_mac_tx
- 接收：main.cpp 的 on_mesh_rx 里 f.frag 时 `frag_reassemble(f.orig_sysid, ...)`，done 回调 mav_router_feed

### 未实现（预留）
- NACK 缺件补发（frag.h 无 NACK 接口，P3 先做重组，缺件补发后续加）
