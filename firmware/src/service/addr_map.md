# addr_map —— sysid ↔ node 对照表

## 职责
维护「MAVLink 系统标识 ↔ 无线节点地址」两套地址的映射（架构 §4.2、§7），供路由和链路质量显示。

## 依赖
- 无硬件依赖；被 `mav_router`（学习）、`mesh`（找路）、`stats`（显示）使用

## 学习机制
- 收到 HEARTBEAT 时 `addr_map_learn(msg.sysid, 帧.src_id)`
- 超时（如 30s）未再见的条目标记失效

## 用途
1. 单播路由：把「发给 sysid=X」翻译成「发到 node_id=addr_map_node(X)」
2. 链路质量：`stats` 命令据此显示「每架飞机（sysid）的信号/上次在线时间」

## 对外接口
见 `addr_map.h`。

## 验收（P3）
- 2 机同时在线时，表里两条 `sysid→node_id` 映射正确，`stats` 能看到两架飞机的链路质量

## 当前实现架构（供人和子 agent 阅读）

### 一句话
维护"MAVLink 系统号 ↔ 无线节点号"的对照表——两套地址体系各管各的（架构 §4.2），这张表把它们对上。

### 数据结构
- `addr_entry_t`：{sysid, node_id, last_seen_ms}，固定 32 槽（ADDR_MAP_MAX）

### 接口
- `addr_map_learn(sysid, node_id)`：从 HEARTBEAT 学（mesh_rx_cb 里调用）
- `addr_map_node(sysid)` → node_id；`addr_map_sysid(node_id)` → sysid（查不到返回 0）
- `addr_map_export(out, max)`：导出表（stats/CLI 显示链路质量）

### 学习时机
main.cpp 的 `on_mesh_rx` 里，每收一个帧：`if (f.orig_sysid != 0) addr_map_learn(f.orig_sysid, f.src_id)`
——即"帧头 orig_sysid 是 MAVLink 号，src_id 是无线节点号，对上"。

### 用途
1. 单播路由：把"发给 sysid=X"翻译成"发到 node_id=addr_map_node(X)"（预留，当前 mesh 用泛洪）
2. 链路质量显示：stats 据此列出每架飞机的信号/在线时间
