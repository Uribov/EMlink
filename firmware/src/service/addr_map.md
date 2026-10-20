# addr_map —— sysid ↔ node 对照表

## 职责

维护「MAVLink 系统标识 ↔ 无线节点地址」两套地址的映射（架构 §4.2、§7），供路由和链路质量显示。表项带老化（P1-9），不会把包单播给已离网的节点号。

两套地址各管各的：飞控只认 sysid，空口只认 node_id，这张表负责换算。


## 依赖

- 无硬件依赖；被 `mav_router`（学习）、`mesh`（找路）、`stats`/`cli`（显示）使用

纯内存表，没有任务，读写都靠一把 portMUX。


## 学习机制

- 收到 HEARTBEAT 时 `addr_map_learn(msg.sysid, 帧.src_id)`
- 超过 `ADDR_ENTRY_TTL_MS`（默认 60s，心跳 1Hz）未再见的条目视为失效：查询返回 0，导出时跳过
- 32 槽满时：先清过期，仍满则淘汰 `last_seen_ms` 最旧的一条；计数 `addr_map_expired_count()` / `addr_map_evict_count()`

学习是被动的，靠飞控自己的心跳维持新鲜度，不需要额外的探测。


## 用途

1. 单播路由：把「发给 sysid=X」翻译成「发到 node_id=addr_map_node(X)」
2. 链路质量：`stats` 命令据此显示「每架飞机（sysid）的信号/上次在线时间」

两个用途都建立在同一次学习上：收到帧时记下对应关系，查询时直接用。


## 对外接口

见 `addr_map.h`。要点：`addr_map_learn / addr_map_node / addr_map_sysid / addr_map_export` + 两个诊断计数。

对外三个查询函数加一个导出，没有状态机。


## 验收（P3）

- 2 机同时在线时，表里两条 `sysid→node_id` 映射正确，`stats` 能看到两架飞机的链路质量
- 某机断链 60s 后：`addr_map_node(它的 sysid)` 返回 0（不再把包单播给离网节点），`addr_map_expired` 增长

两条分别验学习和老化。


---

## 当前实现架构（供人和子 agent 阅读）

### 简介

维护 MAVLink 系统号与无线节点号的对照表：两套地址体系各管各的（架构 §4.2），这张表把它们对上。


### 数据结构

- `addr_entry_t`：{sysid, node_id, last_seen_ms}，固定 32 槽（ADDR_MAP_MAX）
- `ADDR_ENTRY_TTL_MS` = 60000（心跳 1Hz，60s 未听到即认为离网）
- `g_expired` / `g_evict`：老化与淘汰计数
- `g_addr_mux`：portMUX 锁，保护 `g_map`（跨核：core0 学习 + core1 导出）

固定 32 槽数组，无分配；core1 也会导出（stats 显示），所以需要 portMUX。


### 接口

- `addr_map_learn(sysid, node_id)`：从 HEARTBEAT 学（mesh_rx_cb 里调用）
  - 满了先 `addr_purge_expired_locked()` 清过期；仍满 → 就地覆写最旧条目（`g_evict++`）
- `addr_map_node(sysid)` → node_id；`addr_map_sysid(node_id)` → sysid（查不到返回 0）
  - 命中但已过期 → `g_expired++`、把被查的键字段置 0、返回 0
- `addr_map_export(out, max)`：导出表（stats/CLI 显示链路质量），跳过过期条目

查询撞到过期条目时就地清掉，省一次后续判断。


### 学习时机

main.cpp 的 `on_mesh_rx` 里，每收一个帧：`if (f.orig_sysid != 0) addr_map_learn(f.orig_sysid, f.src_id)`
—— 即「帧头 orig_sysid 是 MAVLink 号，src_id 是无线节点号，对上」。


### 用途

1. 单播路由：把「发给 sysid=X」翻译成「发到 node_id=addr_map_node(X)」（预留，当前 mesh 用泛洪）
2. 链路质量显示：stats 据此列出每架飞机的信号/在线时间
