# route —— 反向路径 + 路由表预留

## 职责
记「谁从哪一跳来」，让单播回信走原路；为将来路由表留位置。**表项带老化**（P1-9），陈旧的下一跳不会被拿去单播。

## 依赖
- 无硬件依赖（纯内存表）
- 被 `mesh`（学习）、`mac`/`mesh`（查询下一跳）、`stats`（邻居表）使用

## 反向路径学习
- `mesh_ingress` 每收一帧，调 `route_learn(src_id, 上一跳邻居)`
- 单播回信时 `route_next_hop(src_id)` 得到上一跳，直接单播给它（省一半空口）

## 路由表（预留，当前不启用）
- 帧类型 `FRAME_ROUTE` 已留位；节点数 >8 再上主动路由（架构 §4.5、§12）
- 本模块只保留 `route_next_hop` 接口，未来可替换为真路由表实现

## 邻居表
- `route_neighbors()` 导出 `node_id/rssi/last_seen`，供 `stats` 命令显示链路质量
- 导出时**跳过已过期条目**（P1-9）

## 老化与容量（P1-9）
- 条目超过 `ROUTE_PATH_TTL_MS`（默认 2 分钟）没再听到 → 失效：`route_next_hop` 返回 0（上层回退广播），`route_neighbors` 不再导出
- 16 槽满时：先清过期，仍满则**淘汰 `seen_ms` 最旧的一条**（原实现是静默丢弃新条目 → 新邻居永远学不到）
- 计数：`route_expired_count()`（查询撞到过期）、`route_evict_count()`（容量淘汰）

## 对外接口
见 `route.h`。要点：`route_learn / route_next_hop / route_update_rssi / route_neighbors` + 两个诊断计数。

## 验收（P2）
- 单向链 A→B→C 传完后，C 回 A 的包能沿原路 B 单播回 A（不泛洪）
- 让某邻居静默 > 2 分钟后：发给它的单播回退广播（不是发进死胡同），`route_expired` 计数增长
- 同场 > 16 个活跃源时新源仍能被学到（`route_evict` 增长）

## 当前实现架构（已实现，供子 agent 分析）

### 数据结构
- `path_t`：{src_id, via（下一跳）, rssi, seen_ms}，固定 16 槽（ROUTE_MAX_NEIGHBORS）
- `ROUTE_PATH_TTL_MS` = 120000（2 分钟）；外场按实测调（太短会让单播频繁退回广播）
- `g_expired` / `g_evict`：老化与淘汰计数
- `g_route_mux`：portMUX 锁，保护 `g_paths`（跨核：core0 学习 + core0/1 查下一跳）

### 接口语义
- `route_learn(src_id, via_neighbor)`：via=0 表示 src 直连（内部存 next=src 自己）；via≠0 存 next=via
  - 满了先 `route_purge_expired_locked()` 清过期；仍满 → 就地覆写最旧条目（`g_evict++`）
- `route_next_hop(src_id)`：返回回 src 的下一跳；无记录返回 0
  - **命中但已过期** → `g_expired++`、`src_id` 置 0、返回 0（避免把单播发进死胡同）
- `route_update_rssi(src_id, rssi)`：更新邻居信号（BEACON 用）
- `route_neighbors(out, max)`：导出邻居表（stats 显示），跳过过期条目

### 反向路径用法（配合 mesh 的 relay_id）
- mesh_task 收广播帧 → `route_learn(f.src_id, f.relay_id)`（relay_id=0 即直连）
- mesh_send 单播 → `route_next_hop(dst)` 定 relay_id（下一跳）；返回 0 则回退广播

### 预留
- 帧类型 FRAME_ROUTE 已留位（架构 §4.5），节点 >8 再上主动路由；当前只用反向路径 + 泛洪兜底
