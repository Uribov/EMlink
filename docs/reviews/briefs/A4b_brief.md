# EMlink 任务书 A-4b：分片重组键加 `src_id`（对应审查日志 P1-9 第四行）

## 背景（为什么改）
`service/frag.cpp:79` 的重组槽查找键是 **`(orig_sysid, frag_seq)`**：

```c
if (g_reasm[i].in_use && g_reasm[i].orig_sysid == orig_sysid && g_reasm[i].frag_seq == hdr->frag_seq) { s = &g_reasm[i]; break; }
```

但 `frag_seq` 是**每个节点自己的** 8 位自增计数器（`frag_split()` 里的 `g_frag_seq++`），各节点互不相同步、只有 256 个取值。于是：

1. **两机同 sysid**（多机最常见的配置错误，A-2 已能检测出来）同时传大包 → 两者的片进同一个槽 → 拼出来的整包是"两架飞机的字节拼接"，**送进飞控就是坏 MAVLink**；
2. 恶意/异常节点只要猜中 `(orig_sysid, frag_seq)` 就能**污染**别人的重组槽（把错片写进去，导致公开的超时丢包）。

槽结构里**本来就存了 `src_id`**（第 14 行，用于 NACK 回发），只是查找键漏了它。修法：键改成 **`(src_id, orig_sysid, frag_seq)`**——收方本来就是靠"哪个邻居发的"来区分数据流，这是最自然的唯一性维度。

顺带（同一个"表满静默退化"问题）：`frag_reassemble()` 里槽满时 `return;`（第 85 行）**静默丢片**，没有任何计数。

## 允许改动的文件（其它一律不碰）
1. `firmware/src/service/frag.h`
2. `firmware/src/service/frag.cpp`
3. `firmware/src/app/cli.cpp`（只加计数显示，见第 3 节）

## 1. `frag.cpp` 查找键加 `src_id`
第 79 行改为：
```c
        if (g_reasm[i].in_use && g_reasm[i].src_id == src_id &&
            g_reasm[i].orig_sysid == orig_sysid && g_reasm[i].frag_seq == hdr->frag_seq) { s = &g_reasm[i]; break; }
```

## 2. `frag.cpp` 槽满时淘汰最旧 + 计数
- 新增 static：`static uint32_t g_slot_evict = 0;`（放在 `g_timeout` 旁边，注释"并发重组槽耗尽、被迫淘汰旧槽的次数"）
- `frag_reassemble()` 里"找空槽"那段改成：
```c
    if (!s) {
        for (int i = 0; i < FRAG_CONCURRENT; i++) {
            if (!g_reasm[i].in_use) { s = &g_reasm[i]; break; }
        }
        if (!s) {
            // 槽满：淘汰开始最早的那个（最可能已无望拼齐），并计数——原实现直接 return，静默丢片
            for (int i = 0; i < FRAG_CONCURRENT; i++) {
                if (!s || (int32_t)(g_reasm[i].start_ms - s->start_ms) < 0) s = &g_reasm[i];
            }
            g_slot_evict++;
        }
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->src_id = src_id;
        s->orig_sysid = orig_sysid;
        s->frag_seq = hdr->frag_seq;
        s->frag_total = hdr->frag_total;
        s->start_ms = now;
    }
```
（注意：`s` 必须在第一个 for 之前初始化为 `nullptr`，淘汰循环里不能再出现 `if (!s) return;`）
- 文件末尾访问器区新增：`uint32_t frag_slot_evict_count() { return g_slot_evict; }`
- `frag_init()` 里把 `g_slot_evict = 0;` 一起清零

## 3. `frag.h`
- 第 30 行注释改为：`// 接收侧：喂入一片，拼齐后回调整包；重组键 =(src_id, orig_sysid, frag_seq)，多机并发不串`
- 统计区新增：`uint32_t frag_slot_evict_count();   // 并发槽耗尽被迫淘汰次数`

## 4. `app/cli.cpp`
`status` 分支里若已有 A-4a 加的"表容量: ..."那一行（`snprintf` 里含 `去重淘汰=` / `路由过期=`），就在**同一个 snprintf 的格式串末尾**追加 ` 分片槽淘汰=%u` 并在参数列表末尾补 `(unsigned)frag_slot_evict_count()`；若那一行不存在，就单独新起一行（同样风格）。
（`frag.h` 若未 include 请补 `#include "../service/frag.h"`）

## 禁止事项
- **不要改线上协议**：帧格式、分片头 4 字节、NACK payload 三字节都不动（`src_id` 本来就在帧头里，收方拿得到）
- 不要动 `frag_split()` 的切片逻辑、不要动 `frag_tick()` 的 NACK 策略
- 不要动 `mav_router.cpp` 的补发缓存（"只有一份"是 P1-9 的另一行，留待后续）
- 不要动任何 `.md`、不要运行 `pio`/编译、不要 `git add/commit`

## 完成后请报告
1. 两个文件各自的改动（"原来 / 现在"）
2. 自答：现在两架 **sysid 都是 1** 的飞机同时各传一个 8 片的大包，接收方会怎么处理？（和改前对比）
3. 自答：`frag_reassemble()` 在 4 个槽都被占用且都未超时的情况下，第 5 个大包的第一片到达时会发生什么？被淘汰的那个槽里 `nack_sent=true` 的欠件会不会再催？（一句话即可，**不要改代码**）
4. 自查：改动后 `g_reasm[i].src_id` 的写入/读取是否都在同一线程（`mesh_task`）上下文中
