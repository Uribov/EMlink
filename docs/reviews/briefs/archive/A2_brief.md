# EMlink 任务书 A-2：`sysid` 接线（对应审查日志 P1-7）

## 背景（为什么改）
`app/role.cpp` 里 AIR 的本机 MAVLink sysid **写死 `MAV_SYSID_FC`(=1)**、GCS 写死 255。
NVS 里的 `sysid`（CLI 可设、网页可见、文档 §9 写着"AIR=飞控号"）在整个 `src/` 里除了读写 NVS 和启动打印之外**没有任何用途**。
后果：两架飞机的 AIR 节点都是 sysid=1 → QGC 里两台飞机被当成同一台；多机场景（架构 §7）不成立。

修法：`role_apply()` 改为"读 NVS `sysid`，为 0 才用角色默认值"；并加 sysid 撞号检测（收到别的节点自称本机 sysid 的帧 → 计数告警，CLI `status` 可见）。

## 硬性约束
- **只允许改/建下面列出的文件**，其它文件一律不碰。
- **不要动任何 `.md` 文档**（文档我来同步）。
- **不要运行编译命令**，**不要 `git commit/add`**。
- 不改函数/变量命名风格；新增接口一律按下面的签名写。
- 「撞号检测」必须**排除自己中继回来的帧**（`src_id == 本机 node_id` 的帧），否则会误报。

## 允许改动的文件
1. `firmware/src/app/role.cpp`
2. `firmware/src/service/mav_router.h`、`firmware/src/service/mav_router.cpp`
3. `firmware/src/net/mesh.h`、`firmware/src/net/mesh.cpp`
4. `firmware/src/main.cpp`
5. `firmware/src/app/cli.cpp`

---

## 1. `app/role.cpp` —— `role_apply()` 读 NVS sysid

把 `role_apply()` 改成：

```c
void role_apply(uint8_t role) {
    g_role = role;
    // 本机 MAVLink sysid：优先用 NVS 配置（sysid=0 表示"按角色默认"），多机靠它区分
    cfg_t c;
    uint8_t sysid = 0;
    if (nvs_cfg_load(&c) && c.sysid != 0) {
        sysid = c.sysid;
    } else if (role == ROLE_AIR) {
        sysid = MAV_SYSID_FC;       // 1
    } else if (role == ROLE_GCS) {
        sysid = MAV_SYSID_GCS;      // 255
    }
    // ROLE_RELAY：保持 0（无 MAVLink 业务）
    mav_router_init(sysid);
}
```
（`role.h` 已 include `nvs_cfg.h`，不需要新增 include。）

## 2. `service/mav_router.h` / `mav_router.cpp` —— 本机 sysid 查询 + 撞号计数

`mav_router.h` 新增三个声明（放在 `mav_router_init` 附近）：

```c
// 本机 MAVLink sysid（0 = 无 MAVLink 业务）
uint8_t mav_router_local_sysid(void);

// 远端节点自报的 sysid（由 main.cpp 的 on_mesh_rx 喂入）：与本机相同 → 撞号计数
void mav_router_note_peer_sysid(uint8_t peer_sysid);

// sysid 撞号次数（多机配置错误的现场证据）
uint32_t mav_router_sysid_conflict_count(void);
```

`mav_router.cpp`：
- 在 `static uint8_t g_local_sysid = 0;` 旁边加 `static uint32_t g_sysid_conflict = 0;`
- `mav_router_init()` 里把 `g_sysid_conflict = 0;` 一并清零
- 实现：

```c
uint8_t mav_router_local_sysid(void) { return g_local_sysid; }

void mav_router_note_peer_sysid(uint8_t peer_sysid) {
    if (g_local_sysid != 0 && peer_sysid == g_local_sysid) g_sysid_conflict++;
}

uint32_t mav_router_sysid_conflict_count(void) { return g_sysid_conflict; }
```

## 3. `net/mesh.h` / `mesh.cpp` —— 暴露本机 node_id

`mesh.h` 新增：

```c
// 本机无线节点号（撞号检测要拿它排除"自己中继回来的帧"）
uint8_t mesh_my_id(void);
```
`mesh.cpp`：在其它 `mesh_*` getter 附近（`mesh_id_conflict_count()` 那一组）加：

```c
uint8_t mesh_my_id(void) { return g_my_node_id; }
```

## 4. `main.cpp` —— `on_mesh_rx` 里喂 sysid 给 mav_router

在 `on_mesh_rx()` 中**紧接 `addr_map_learn` 那一行之后**插入：

```c
    // sysid 撞号检测：远端节点用着与本机相同的 MAVLink sysid（多机最常见的配置错误）
    // 排除自己中继回来的帧（src_id == 本机 node_id）
    if (f->orig_sysid != 0 && f->src_id != mesh_my_id()) mav_router_note_peer_sysid(f->orig_sysid);
```

## 5. `app/cli.cpp` —— `status` 里显示本机 sysid 与撞号计数

在现有 `node_id=... 撞号冲突=...` 那一行**之后**插入一行：

```c
        snprintf(line, sizeof(line), "sysid=%u（本机 MAVLink 号）  sysid撞号=%u\r\n",
                 (unsigned)mav_router_local_sysid(), (unsigned)mav_router_sysid_conflict_count());
        cli_print(line);
```
（`cli.cpp` 若未 include `../service/mav_router.h` 则补上。）

## 完成后请报告
- `git diff --stat`
- 每个文件改了什么（中文简述）
- 你如何确保"自己中继回来的帧"不会被误判为撞号
