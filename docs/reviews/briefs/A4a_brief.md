# EMlink 任务书 A-4a：三张表的老化淘汰与可见性（对应审查日志 P1-9 前三行）

## 背景（为什么改）
三张固定容量表要么"**满了就静默退化**"，要么"**只增不淘汰**"，外场都是灾难：

| 结构 | 现状 | 后果 |
|---|---|---|
| mesh 去重表（16 源） | `mesh.cpp` 的 `dedup_slot()` 满时返回 `nullptr`，`dedup_check()` 当作"新帧" | **不再判重** → 重复帧被反复转发，广播风暴成倍放大 |
| 路由表（16 条，`route.cpp`） | 只增不淘汰，`seen_ms` 只写不读 | 移动网络中陈旧条目占满 → 新路径永远学不进来 |
| `addr_map`（32 条） | `last_seen_ms` 写了**从不使用** | 同上；且会把包单播给**早已离网**的 node_id |

统一修法：**老化 TTL + 满时 LRU 淘汰 + 计数（CLI 可见）**，并且"过期条目"在**查询路径**上就要失效（不能只靠插入时清理）。

## 允许改动的文件（其它一律不碰）
1. `firmware/src/net/mesh.h`、`firmware/src/net/mesh.cpp`
2. `firmware/src/net/route.h`、`firmware/src/net/route.cpp`
3. `firmware/src/service/addr_map.h`、`firmware/src/service/addr_map.cpp`
4. `firmware/src/app/cli.cpp`

## 1. `net/mesh.cpp` 去重表：满时淘汰最久未见的那一源
`dedup_slot()` 改为：

```c
// 找源对应的槽；没有空槽时淘汰"最久未见"的源（原实现返回 nullptr → 整表停止判重）
static dedup_slot_t* dedup_slot(uint8_t src_id) {
    dedup_slot_t* free_slot = nullptr;
    dedup_slot_t* oldest    = nullptr;
    for (int i = 0; i < MESH_MAX_SRC; i++) {
        if (g_dedup[i].src_id == src_id) return &g_dedup[i];
        if (g_dedup[i].src_id == 0) { if (!free_slot) free_slot = &g_dedup[i]; continue; }
        if (!oldest || (int32_t)(g_dedup[i].last_seen_ms - oldest->last_seen_ms) < 0) oldest = &g_dedup[i];
    }
    if (free_slot) return free_slot;
    if (oldest) {           // 表满：淘汰最旧的源
        oldest->src_id = 0; // 置空，交给 dedup_check 重新初始化
        g_dedup_evict++;
        return oldest;
    }
    return nullptr;
}
```

- 新增 static 计数：`static uint32_t g_dedup_evict = 0;`（放在 `g_fwd/g_drop_*` 那一行后面，单独一行，注释写"去重表满淘汰次数（>0 说明同场活跃源超过 MESH_MAX_SRC）"）
- `net/mesh.h` 统计区新增：`uint32_t mesh_dedup_evict_count();`
- `net/mesh.cpp` 访问器区新增：`uint32_t mesh_dedup_evict_count() { return g_dedup_evict; }`
- `dedup_check()` 的 `if (!s) return false;` **保留**（作为理论兜底），但注释改成"不可能到达：dedup_slot 已保证有槽"

## 2. `net/route.{h,cpp}` 路由表：TTL 老化 + 满时 LRU
- `route.h` 新增常量：
```c
// 路径老化：这么久没再听到该源即视为失效（外场按实测调；太短会让单播频繁退回广播）
#define ROUTE_PATH_TTL_MS 120000
```
- `route.h` 新增接口：
```c
// 因过期/满淘汰而失效的路径条数（诊断用）
uint32_t route_expired_count();
uint32_t route_evict_count();
```
- `route.cpp` 新增两个 static：`static uint32_t g_expired = 0, g_evict = 0;`
- 新增模块内 static 辅助（在 `route_learn` 之前）：
```c
// 丢掉已过期的条目（压缩数组），返回是否仍满
static bool route_purge_expired_locked(uint32_t now) {
    for (int i = 0; i < (int)g_n; ) {
        if (now - g_paths[i].seen_ms > ROUTE_PATH_TTL_MS) {
            g_paths[i] = g_paths[--g_n];   // 尾部搬过来
            memset(&g_paths[g_n], 0, sizeof(path_t));
            g_expired++;
        } else i++;
    }
    return g_n >= ROUTE_MAX_NEIGHBORS;
}
```
（注意：调用时必须在 `portENTER_CRITICAL(&g_route_mux)` 之内）
- `route_learn()`：找不到已有条目时，**先 `route_purge_expired_locked(millis())`**；若仍满，淘汰 `seen_ms` 最旧的条目（`g_evict++`）后占用它；否则追加。
- `route_next_hop()`：命中条目时先判过期——过期就 `g_expired++`、置 `src_id=0`、返回 0（**这条最关键**：陈旧下一跳会把单播发进死胡同；返回 0 = 未知，上层会退回广播）。
- `route_neighbors()`：跳过 `now - seen_ms > ROUTE_PATH_TTL_MS` 的条目（不导出陈旧邻居）。
- 所有读写仍必须在 `g_route_mux` 临界区内（别在新增代码里漏掉）。

## 3. `service/addr_map.{h,cpp}`：同上一套
- `addr_map.h` 新增：`#define ADDR_ENTRY_TTL_MS 60000`（心跳 1Hz，60s 没听到即认为该 sysid 已不在网）+ 两个计数接口 `uint32_t addr_map_expired_count();` `uint32_t addr_map_evict_count();`
- `addr_map.cpp`：`static uint32_t g_expired = 0, g_evict = 0;` + 模块内 static `addr_purge_expired_locked(now)`（与 route 同构）
- `addr_map_learn()`：插入前先清过期，仍满则淘汰 `last_seen_ms` 最旧条目（`g_evict++`）
- `addr_map_node()`：命中但过期 → `g_expired++`、置该条 `sysid=0`、返回 0（**避免把包单播给已离网的 node_id**）
- `addr_map_sysid()`：同样处理
- `addr_map_export()`：跳过过期条目
- 同一套 `g_addr_mux` 临界区纪律

## 4. `app/cli.cpp`（status 分支）
在已有的"认证失败/占空比"等行之后追加**一行**（整行 ≤100 字符，注意 `line[]` 缓冲大小）：
```c
            snprintf(line, sizeof(line), "表容量: 去重淘汰=%u 路由过期=%u/%u 地址过期=%u/%u\r\n",
                     (unsigned)mesh_dedup_evict_count(),
                     (unsigned)route_expired_count(), (unsigned)route_evict_count(),
                     (unsigned)addr_map_expired_count(), (unsigned)addr_map_evict_count());
            cli_print(line);
```
（`route.h` / `addr_map.h` 若未 include 请补上）

## 禁止事项
- 不要动 `app/stats.cpp` / `stats.h`（这些计数暂不进 MAVLink RADIO_STATUS，那是 P1-10）
- 不要改去重算法本身（窗口/位图逻辑）、不要改 route 的 `via` 语义
- 不要动任何 `.md`、不要运行 `pio`/编译、不要 `git add/commit`

## 完成后请报告
1. 六个文件各自的改动（"原来 / 现在"）
2. 自答：**同场出现第 17 个活跃源**时，"去重"和"转发"分别会发生什么？（与旧行为对比）
3. 自答：`route_next_hop()` 遇到一条 3 分钟前学到的路径，返回什么？上层（`mav_router`/`frag` 的单播路径）拿到 0 会怎么走？
4. 自查：新增的读写有没有哪一处跑到了 `portMUX` 临界区外面（列出你检查过的函数）
