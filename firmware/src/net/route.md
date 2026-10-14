# route —— 反向路径 + 路由表预留

## 职责
记「谁从哪一跳来」，让单播回信走原路；为将来路由表留位置。

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

## 对外接口
见 `route.h`。

## 验收（P2）
- 单向链 A→B→C 传完后，C 回 A 的包能沿原路 B 单播回 A（不泛洪）

## 当前实现架构（已实现，供子 agent 分析）

### 数据结构
- `path_t`：{src_id, via（下一跳）, rssi, seen_ms}，固定 16 槽（ROUTE_MAX_NEIGHBORS）
- `g_route_mux`：portMUX 锁，保护 `g_paths`（跨核：core0 学习 + core0/1 查下一跳）

### 接口语义
- `route_learn(src_id, via_neighbor)`：via=0 表示 src 直连（内部存 next=src 自己）；via≠0 存 next=via
- `route_next_hop(src_id)`：返回回 src 的下一跳；无记录返回 0
- `route_update_rssi(src_id, rssi)`：更新邻居信号（BEACON 用）
- `route_neighbors(out, max)`：导出邻居表（stats 显示）

### 反向路径用法（配合 mesh 的 relay_id）
- mesh_task 收广播帧 → `route_learn(f.src_id, f.relay_id)`（relay_id=0 即直连）
- mesh_send 单播 → `route_next_hop(dst)` 定 relay_id（下一跳）；返回 0 则回退广播

### 预留
- 帧类型 FRAME_ROUTE 已留位（架构 §4.5），节点 >8 再上主动路由；当前只用反向路径 + 泛洪兜底
