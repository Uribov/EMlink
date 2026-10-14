# mesh —— 泛洪转发 / 去重 / TTL

## 职责
Mesh 核心：决定「收到一帧后，转发还是丢，还是上交本机」。

## 依赖
- `frame_codec.h`（`frame_t`）
- `route.h`（转发时顺带学反向路径）
- 被 `mac`（转发出）、`mav_router`/`frag`（收包回调）使用

## 转发判定（`mesh_ingress`）
1. **去重**：键 `(src_id, seq)`。每源一个 256 槽滑动窗（bitmap + `last_seq` 锚点，回绕处理）。命中 → 丢弃，计 `dropped_dup`
2. **TTL**：`ttl==0` → 丢弃，计 `dropped_ttl`；否则 `ttl--`
3. **投递本机**：`dst_id` 是自己或广播 → 走 `mesh_rx_cb` 上交（rssi/snr 一并）
4. **转发**：`dst_id==0`（广播）且 `ttl>0` → 转发出，计 `fwd`
   单播且 `dst_id != 自己` → 经 `route_next_hop` 转发（无路径则回退泛洪）

## 去重实现（关键）
- 每源：`last_seq`（最近序号）+ 256-bit 位图，位 `(last_seq - seq) & 255` 标记已见
- 收到 `seq` 在窗口内且位已置 → 重复；新 `seq` 前移 `last_seq` 并滚动位图
- 槽位 30s 无更新自动作废（`MESH_DEDUP_TTL_MS`），防内存漂移

## 反向路径（配合 route）
收到帧时调 `route_learn(f->src_id, 上一跳)`，回信单播省一半空口（架构 §4.5）。

## 对外接口
见 `mesh.h`。

## 验收（P2）
- 3 节点链式（A–B–C）广播，A/C 经 B 可达；同一帧 B 只转发一次（去重生效）
- 环形拓扑无死循环（TTL 兜底），包数对得上

## 当前实现架构（已实现，供子 agent 分析）

### static 变量
- `g_dedup[16]`：去重表（每源 last_seq + 256bit 位图 + last_seen）
- `g_my_node_id` / `g_my_ttl` / `g_seq`：本机地址 / 默认 TTL / 发送序号
- `g_rx_cb`：收包回调（main.cpp 设，接 mav_router / frag / addr_map）
- 统计：`g_fwd` / `g_drop_ttl` / `g_drop_dup` / `g_drop_crc` / `g_rx` / `g_last_rx_ms`

### mesh_task 收帧流程（按序）
1. `frame_decode`（CRC 失败 → drop_crc）
2. ACK 帧 → `mac_on_ack_rx`，continue
3. 去重 `mesh_ingress`（命中 → drop_dup）
4. BEACON 帧 → `route_learn` + `route_update_rssi`，continue
5. 数据帧且 ack_req → `mac_send_ack`
6. 广播帧 → `route_learn(src, relay_id)` 学反向路径
7. 投递本机（dst=0 或 dst=自己）→ `g_rx_cb`
8. 转发判定（见下）

### 转发判定（relay_id 语义，架构 §4.5）
- 广播（dst=0）：人人转发一次，relay=本机
- 单播（dst≠自己）：仅 relay==本机（我是下一跳）才转发；查 `route_next_hop(dst)`，未知回退广播
- 转发时 ttl--，**保持原 ack_req（逐跳 ACK）**

### 去重实现
- 键 (src_id, seq)；每源 last_seq + 256bit 位图；位 diff=last_seq-seq
- seq 前进时位图左移 diff 位；槽 30s 未动重置（MESH_DEDUP_TTL_MS）

### mesh_send / mesh_send_beacon
- `mesh_send(dst,qos,ack,payload,len,orig_sysid)`：单播查 route_next_hop 定 relay_id，未知回退广播
- `mesh_send_beacon()`：FRAME_BEACON，空 payload，ttl=1（只一跳不转发），proto=CUSTOM
