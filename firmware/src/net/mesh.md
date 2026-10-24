# mesh —— 泛洪转发 / 去重 / TTL

## 职责

Mesh 核心：决定「收到一帧后，转发还是丢，还是上交本机」。

三个判断按顺序做：先去重、再看 TTL、最后决定转发还是上交。


## 依赖

- `frame_codec.h`（`frame_t`）
- `route.h`（转发时顺带学反向路径）
- 被 `mac`（转发出）、`mav_router`/`frag`（收包回调）使用

依赖集中在帧结构与反向路径，收包回调由 main.cpp 注入。


## 转发判定（`mesh_ingress`）

1. 去重：键 `(src_id, seq)`。每源一个 256 槽滑动窗（bitmap + `last_seq` 锚点，回绕处理）。命中 → 丢弃，计 `dropped_dup`
2. TTL：`ttl==0` → 丢弃，计 `dropped_ttl`；否则 `ttl--`
3. 投递本机：`dst_id` 是自己或广播 → 走 `mesh_rx_cb` 上交（rssi/snr 一并）；加密帧先解密，认证失败只跳过本机投递（计 `drop_auth`）—— 不停第 4 步的转发（P1-8）
4. 转发：`dst_id==0`（广播）且 `ttl>0` → 转发出，计 `fwd`
   单播且 `dst_id != 自己` → 经 `route_next_hop` 转发（无路径则回退泛洪）

四步的顺序不能调：去重必须在转发之前，否则重复帧会被放大。


## 去重实现

- 每源：`last_seq`（最近序号）+ 256-bit 位图，位 `(last_seq - seq) & 255` 标记已见
- 收到 `seq` 在窗口内且位已置 → 重复；新 `seq` 前移 `last_seq` 并滚动位图
- 槽位 30s 无更新自动作废（`MESH_DEDUP_TTL_MS`），防内存漂移
- 表满（16 源）→ 淘汰最久未见的槽并计 `g_dedup_evict`（P1-9；原实现返回 nullptr＝不再判重，重复帧会被放大转发）

去重表是 16 源 × 256 位的固定结构，不做动态分配，所以容量和内存占用都是可预期的。


## 反向路径（配合 route）

收到帧时调 `route_learn(f->src_id, 上一跳)`，回信单播省一半空口（架构 §4.5）。

学习是被动的：不需要额外的探测帧，任何收到的帧都顺带更新一遍。


## 对外接口

见 `mesh.h`。要点：

- `mesh_my_id()`：本机 node_id（撞号检测要拿它排除「自己中继回来的帧」，P1-7）
- `mesh_id_conflict_count()`：node_id 撞号计数；`mesh_drop_crc_count()` / `mesh_drop_auth_count()` 等收发统计
- 数据帧诊断计数（F21）：`mesh_tx_data_count()` / `mesh_rx_data_count()` / `mesh_deliver_count()`，`status` 输出为「数据帧: 发=N 收=N 投递=N」

对外分三组：身份查询、收发计数、F21 数据帧诊断。


## 验收（P2）

- 3 节点链式（A–B–C）广播，A/C 经 B 可达；同一帧 B 只转发一次（去重生效）
- 环形拓扑无死循环（TTL 兜底），包数对得上
- 密钥不一致的节点：本机收不到明文（`drop_auth` 计数增长），但转发照旧（P1-8 验收）

三条分别验转发、防环和信任边界。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

收到一帧后决定三件事：是不是重复、该不该转发、要不要上交本机。全网靠泛洪 + 去重 + TTL 兜底，不需要路由收敛。


### static 变量

- `g_dedup[16]`：去重表（每源 last_seq + 256bit 位图 + last_seen）
- `g_my_node_id` / `g_my_ttl`：本机地址 / 默认 TTL
- `g_seq` + `g_seq_mux`：发送序号 + portMUX 锁（全系统唯一 seq 源，见下）
- `g_rx_cb`：收包回调（main.cpp 设，接 mav_router / frag / addr_map）
- 统计：`g_fwd` / `g_drop_ttl` / `g_drop_dup` / `g_drop_crc` / `g_drop_auth`（P1-8）/ `g_rx` / `g_last_rx_ms`
- 数据帧诊断计数（F21，`static volatile uint32_t`，只增不减）：`g_tx_data_count`（`mesh_send` 成功入队）/ `g_rx_data_count`（收到 `FRAME_DATA`）/ `g_deliver_count`（投递给 `g_rx_cb`）
- 撞号检测（P0-4）：`g_my_seq_hist[32]`（本机最近发出的 seq 环，`mesh_next_seq` 内写入）+ `g_id_conflict` 计数
- 去重表淘汰计数（P1-9）：`g_dedup_evict`

前四组是运行状态，后面是三类计数（收发统计、F21 诊断、撞号与淘汰）。


### 序号分配（mesh_next_seq，跨核原子）

- `mesh_next_seq()` 用 portMUX 保护 `g_seq++`，是全系统唯一 seq 源
- 普通帧/分片帧/控制帧/beacon/ping 都从它取 seq，禁止任何模块自建 seq 计数器（多计数器会导致同一节点 seq 重叠 → 去重误判丢帧）

集中到一个函数取号，是为了让去重表看到的序号严格单调。


### mesh_task 收帧流程（按序）

1. `frame_decode`（CRC 失败 → drop_crc）
1b. 撞号检测（P0-4）：`src_id == 本机 && !seq_is_mine(seq)`（陌生帧自称本机号）→ `g_id_conflict++`
2. ACK 帧 → `mac_on_ack_rx` + 匹配 ping 等待（`g_ping_wait_seq`），continue
3. 去重 `mesh_ingress`（命中 → drop_dup）
4. BEACON 帧 → `route_learn(src, relay_id)` + `route_update_rssi` + 取 `sender_role`（`payload_len>=2 ? payload[1] : ROLE_GCS`）→ `adr_on_beacon(src, relay_id, sender_role)`，不 continue → 继续走到转发判定（`dst=0`、`ttl>0` → 中继转发一次，P1-3：信标可多跳）
5. PING 帧 → 回 ACK，不投递不转发，continue
6. 数据帧且 ack_req → `mac_send_ack`
7. 广播帧 → `route_learn(src, relay_id)` 学反向路径
8. 投递本机（dst=0 或 dst=自己；类型 DATA/NACK/CFG）：`enc` 帧先 `crypto_open_frame` 解密；成功才 `g_rx_cb`（用 `rx` 副本、明文替换 payload，并 `g_deliver_count++`）；失败 `g_drop_auth++` 且只跳过投递（P1-8：原来是 `continue`，会连带吞掉转发）。解密成功后还要过 `crypto_check_replay(src_id, seq)`（P1-4 收端重放窗），不通过同样只跳过投递、转发照旧
9. 转发判定（密文原样转发，用原帧 `f`；转发不需要密钥）
- 循环带 100ms 超时收帧，超时也调 `frag_tick()`（缺件 NACK 检查）
- 任务开头 `watchdog_register_current()` 订阅任务看门狗；循环每轮 `watchdog_feed()`（含 100ms 超时走 frag_tick 的分支，P0-1）

九个步骤串成一条收帧流水线，其中只有 BEACON 一条会同时投递和转发。


### 转发判定（relay_id 语义，架构 §4.5）

- 广播（dst=0）：人人转发一次，relay=本机
- 单播（dst≠自己）：仅 relay==本机（我是下一跳）才转发；查 `route_next_hop(dst)`，未知回退广播
- 转发时 ttl--，保持原 ack_req（逐跳 ACK）

广播靠人传人，单播靠反向路径指定的下一跳，两条路互不干扰。


### 信任边界（P1-8）

- 中继只搬密文：CRC 通过即可转发，不解密、不看 payload → 无密钥的中继不是黑洞
- 唯一例外：`frame_decode` 失败（CRC 错）确实无法转发（帧本身不可用，计 `drop_crc`）
- 认证失败不等于坏帧：帧结构完整，只是本机解不开 → 计数 + 不投递 + 继续转发
- ACK 仍在认证之前发出（P1-1 待办，不在本次范围）

这条边界的关键在于把「我解不开」和「帧坏了」分开：前者照样转发，后者才丢。


### BEACON payload 兼容（v3）

- 新格式：`payload[0]` = 协议版本，`payload[1]` = 发送者角色（`ROLE_*`）
- 接收端 `payload_len >= 2` 才读角色；只有 1 字节（旧固件）时默认 `ROLE_GCS`，保持旧固件之间原有的「信标=锚点」行为
- 版本号仍只用于比对告警，不拒绝帧；新旧混跑时旧 AIR 信标会被当成锚点，所以升级需 GCS/AIR 两端一起刷


### 去重实现

- 键 (src_id, seq)；每源 last_seq + 256bit 位图；位 diff=last_seq-seq
- seq 前进时位图左移 diff 位；槽 30s 未动重置（MESH_DEDUP_TTL_MS）
- 槽满（16 源）→ 取 `last_seen_ms` 最旧者、清零后复用（`g_dedup_evict++`）
- 淘汰安全性：清 `src_id=0` 后 `dedup_check` 会按「新源」重新初始化，不会把新源的 seq 误当旧源历史


### mesh_send / mesh_send_ctrl / mesh_send_beacon / mesh_ping

- `mesh_send(dst,qos,ack,payload,len,orig_sysid)`：发 FRAME_DATA，单播查 route_next_hop 定 relay_id，未知回退广播；len 上限 175，frame_encode 前 `crypto_seal_frame` 加密
- `mesh_send_ctrl(type,dst,qos,ack,payload,len)`：发控制帧（FRAME_NACK/FRAME_CFG），同样走加密 + relay 逻辑
- `mesh_send_beacon(sender_role)`：FRAME_BEACON，payload = 2 字节 `{协议版本 EM_PROTO_VERSION, 发送者角色}`（v3），`ttl = MESH_BEACON_TTL`(=2：GCS 发 + 最多 1 跳转发，P1-3)，proto=CUSTOM（不加密）。兼容规则：旧固件 `payload_len==1` → 接收端按 `ROLE_GCS` 处理，退回旧行为（当成 ADR 锚点）；新固件只有 GCS 发的信标参与对齐，AIR 的 2 字节信封只算「在场」
- `mesh_ping(dst,timeout,rssi_out)`：发 FRAME_PING 等 ACK 测 RTT，阻塞轮询 `g_ping_done`（mesh_task 收到匹配 ACK 时置位）
- 四个发送函数 seq 都走 `mesh_next_seq()`（不直接碰 `g_seq`）

四个发送函数共用同一条取号 + 加密 + 选路的路径，差别只在帧类型和载荷。
