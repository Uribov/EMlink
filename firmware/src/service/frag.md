# frag —— 分片与重组

## 职责

MAVLink 大消息（>175B）拆成小片、对方拼回；缺件只补缺件。遥测/心跳/单航点（≤38B）不分片。

分片对上层透明：mav_router 只管把整条消息丢进来，拆和拼都在这里完成。


## 依赖

- `frame_codec.h`（`FRAME_MAX_PLAIN=175`、`FLAG_FRAG`、`frag_hdr[4]`）

依赖只有帧结构定义，不碰硬件也不碰射频。


## 分片（发送侧）

- `len > FRAME_MAX_PLAIN` 时拆；分片头写 `frag_hdr[4]`（msg_id/index/total/seq）
- 每片 payload ≤175B，片序 `frag_index` 0..total-1，组标识 `frag_seq` 自增

拆出来的每一片都是独立的完整帧，可以单独加密、单独重传。


## 重组（接收侧）

- 键 = `(src_id, orig_sysid, frag_seq)`，按 `frag_index` 存到重组缓冲（P1-9：漏 `src_id` 会让同 sysid 的两机串包）
- 全片到齐 → 拼接回调 `done(full, len)`
- 缺件：收第一片后 500ms 还没拼齐 → 回 NACK（`[orig_sysid, frag_seq, 缺失位图]`），发方只补缺的那几片
- 2s 未拼齐 → 整组作废，计 `frag_timeout_count`（NACK 补发也失败时的兜底）
- 4 个并发槽全占又来了新包 → 淘汰 `start_ms` 最早（最可能无望）的槽，计 `frag_slot_evict_count`（P1-9；原实现直接丢片）

重组的三条时间线：500ms 催补、2s 作废、槽满淘汰，各自负责一种失败情形。


## 片大小 175 的由来（加密预留）

加密（crypto 模块）会给每片 payload 加 4B tag，故片大小从 179 降到 175（`FRAME_MAX_PLAIN`）：
加密后每片密文 = 175+4 = 179，正好塞满 payload 区。未加密时也按 175 切（逻辑统一，少 4B 空间换一份代码）。

175 这个数是按「加密后正好填满 payload 区」倒推的。


## 内存注意

重组缓冲预估几 KB（4 并发 × 267B），放 SRAM 即可，不放栈；PSRAM 有余量也可用。

缓冲放在静态区而不是栈上，因为 4 槽并发就是几 KB，栈放不下。


## 对外接口

见 `frag.h`。要点：`frag_split / frag_reassemble / frag_tick` + 统计 `frag_timeout_count / frag_slot_evict_count`。

对外三个函数加两个计数，`frag_tick` 由 mesh_task 周期调用。


## 验收（P3）

- 单条 267B MAVLink 2 消息（无签名最大帧）→ 拆 2 片 → 对端无损重组
- 人为丢 1 片 → NACK → 发方只补该片 → 重组成功
- 两机 sysid 相同（配置撞号）同时发大包 → 各自拼齐，不串包（P1-9）
- 4 槽占满后第 5 个包仍能开始重组（`frag_slot_evict` 增长，被淘汰的包 2s 后自然作废）

四条里的后两条是并发与容量边界。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

把超过 175 字节的 MAVLink 消息切开传，对端拼回来；丢片时只补缺的那几片，不重发整包。


### 分片头 frag_hdr_t（4B，帧偏移 11~14，P1-4 后帧头 15B）

`msg_id`（占位 0）+ `frag_index`（第几片）+ `frag_total`（总片数）+ `frag_seq`（组序号自增）


### 发送侧 frag_split

- len ≤ 175 单片（返回 1）；>175 拆 ceil(len/175) 片
- hdr 输出公共部分（frag_seq 自增 / frag_total），frag_index 由调用者每片填

拆片时只算一次公共头，逐片只改 index。


### 接收侧 frag_reassemble（键 =(src_id, orig_sysid, frag_seq)）

- 重组槽 `reasm_slot_t`：4 并发（FRAG_CONCURRENT），每槽 8 片 × 175B
- 槽匹配比 src_id + orig_sysid + frag_seq 三个字段（多机并发不串包；`src_id` 是发片邻居的节点号，转发不改它）
- 槽记录 `src_id`（发片源节点）+ `nack_sent`（防 NACK 风暴，只催一次）
- 位图 got 判拼齐；拼齐回调 `done(full, len)`
- 2s 超时懒清理（FRAG_REASM_TIMEOUT_MS），计 `frag_timeout_count`
- 槽满淘汰（P1-9）：4 槽全占且无空槽 → 选 `start_ms` 最早的槽，`g_slot_evict++`，`memset` 后作为新包的第 0 槽；被淘汰包不再发 NACK（槽已重建）

超时清理是懒执行的，等下一次操作时顺带做，省一个定时任务。


### 缺件 NACK

- `frag_tick()` 由 mesh_task 每 ~100ms 调（收帧超时也调）
- 收第一片后 ≥ FRAG_NACK_WAIT_MS(500ms) 且未拼齐且没发过 NACK → 发 FRAME_NACK（`mesh_send_ctrl`）
- NACK payload 3B：`[orig_sysid, frag_seq, 缺失位图]`；`nack_sent=true` 只催一次
- 发送侧（mav_router）收到 NACK → 从发送缓存补发缺失片（见 mav_router.md）

NACK 只催一次，避免两边互相刷缺件请求。


### 调用链

- 发送：mav_router 的 route_msg 里 len>FRAME_MAX_PLAIN 时 frag_split + 缓存原始数据 + 逐片 crypto_seal_frame + frame_encode 投 q_mac_tx
- 接收：main.cpp 的 on_mesh_rx 里 f.frag 时 `frag_reassemble(f.src_id, f.orig_sysid, ...)`，done 回调 mav_router_feed
- NACK：frag_tick（mesh_task）→ mesh_send_ctrl(FRAME_NACK) → 对端 on_mesh_rx → mav_router_on_nack 补发
