# frag —— 分片与重组

## 职责
MAVLink 大消息（>180B）拆成小片、对方拼回；缺件只补缺件。遥测/心跳/单航点（≤38B）**不分片**。

## 依赖
- `frame_codec.h`（`FRAME_MAX_PAYLOAD=180`、`FLAG_FRAG`、`frag_hdr[4]`）

## 分片（发送侧）
- `len > FRAME_MAX_PAYLOAD` 时拆；分片头写 `frag_hdr[4]`（msg_id/index/total/seq）
- 每片 payload ≤180B，片序 `frag_index` 0..total-1，组标识 `frag_seq` 自增

## 重组（接收侧）
- 键 = `(orig_sysid, frag_seq)`，按 `frag_index` 存到重组缓冲
- 全片到齐 → 拼接回调 `done(full, len)`
- 缺件：回 NACK（携带缺失位图），发方**只补缺的那几片**，不重发全部
- 2s 未拼齐 → 整组作废，计 `frag_timeout_count`

## 内存注意
重组缓冲预估几 KB（4 并发 × 267B），放 SRAM 即可，不放栈；PSRAM 有余量也可用。

## 对外接口
见 `frag.h`。

## 验收（P3）
- 单条 267B MAVLink 2 消息（无签名最大帧）→ 拆 2 片 → 对端无损重组
- 人为丢 1 片 → NACK → 发方只补该片 → 重组成功
