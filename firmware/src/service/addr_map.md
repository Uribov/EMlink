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
