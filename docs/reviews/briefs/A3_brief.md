# EMlink 任务书 A-3：转发与投递解耦（对应审查日志 P1-8）

## 背景（为什么改）
`net/mesh.cpp` 投递段（约 405~418 行，任务书里给的是 P1-2 修复后的行号，请按代码内容定位）：

```c
        // 投递本机（dst 是自己或广播）；加密帧先解密，认证失败丢弃
        if ((f.type == FRAME_DATA || f.type == FRAME_NACK || f.type == FRAME_CFG) &&
            (f.dst_id == 0 || f.dst_id == g_my_node_id)) {
            frame_t rx = f;
            uint8_t plain[FRAME_MTU];
            if (f.enc) {
                size_t plen = crypto_open_frame(&f, plain, sizeof(plain));
                if (plen == 0) { g_drop_crc++; continue; }   // ← 根因在这一行
```

`continue` 同时干了两件事：① 本机不投递（**正确**）② **整帧不再转发**（**错误**）。
转发路径转发的是**密文原帧**，中继不需要密钥；现在只要一个节点没密钥（或标签校验失败），它就会变成全网广播的**黑洞**（收得到、不转发），与 README / 架构 §13.1 宣称的"中继只转发密文、无需持密钥"自相矛盾。

顺带：认证失败还错记进了 `g_drop_crc`（统计名不副实）。

## 允许改动的文件（其它一律不碰）
1. `firmware/src/net/mesh.h`
2. `firmware/src/net/mesh.cpp`
3. `firmware/src/app/cli.cpp`

## 逐条改法

### 1. `net/mesh.h`
在 `uint32_t mesh_drop_crc_count();` 之后新增：
```c
// 认证/解密失败计数（P1-8：只影响本机投递，不影响转发）
uint32_t mesh_drop_auth_count();
```

### 2. `net/mesh.cpp`
**(a) static 计数区**（`g_drop_crc` 旁边）新增：`static uint32_t g_drop_auth  = 0;`
（注意对齐注释列，与相邻行风格一致）

**(b) 访问器区**（`mesh_drop_crc_count()` 之后）新增：
```c
uint32_t mesh_drop_auth_count() { return g_drop_auth; }
```

**(c) 投递段改写**为"认证失败只跳过本机投递，不跳过转发"：
```c
        // 投递本机（dst 是自己或广播）；加密帧先解密
        // P1-8：认证/解密失败只影响本机投递，不影响转发——转发的是密文，中继不需要密钥
        if ((f.type == FRAME_DATA || f.type == FRAME_NACK || f.type == FRAME_CFG) &&
            (f.dst_id == 0 || f.dst_id == g_my_node_id)) {
            frame_t rx = f;
            uint8_t plain[FRAME_MTU];
            bool deliver = true;
            if (f.enc) {
                size_t plen = crypto_open_frame(&f, plain, sizeof(plain));
                if (plen == 0) {
                    g_drop_auth++;      // 认证失败：本机不投递（但仍按 TTL 转发）
                    deliver = false;
                } else {
                    rx.payload     = plain;
                    rx.payload_len = plen;
                    rx.enc         = false;
                }
            }
            if (deliver && g_rx_cb) g_rx_cb(&rx, (float)item.rssi, (float)item.snr);
        }
```

**硬性注意**：绝对不要就地修改 `f` —— 后面的转发判定与 `frame_encode(&f, ...)` 还要用它，且必须转发**原密文**。只用副本 `rx` 改字段。

### 3. `app/cli.cpp`（status 分支）
紧挨着占空比那行（A-1 新增的 `duty_oversize_count()` 行）之后追加一行：
```c
            snprintf(line, sizeof(line), "认证失败: %u 帧（只影响本机投递，不影响转发）\r\n",
                     (unsigned)mesh_drop_auth_count());
            cli_print(line);
```

## 禁止事项
- 不要动 `app/stats.cpp` / `stats.h`（认证计数暂不进 MAVLink RADIO_STATUS，那是 P1-10 的事）
- 不要改 ACK 相关逻辑（"ACK 先于认证发出"属于 P1-1，不在本次范围）
- 不要动任何 `.md` 文档（我来写）
- 不要运行 `pio`/编译命令，不要 `git add/commit`

## 完成后请报告
1. 三个文件各自改了哪些行（把"原来 / 现在"贴出来）
2. 现在一个**没有密钥的节点**收到加密广播时会依次做哪几步（一句话说清"投递"和"转发"各是什么结果）
3. 顺带自查：`mesh_task_func` 的接收循环里，**还有没有别的地方会因为"帧有问题"而提前 `continue`、从而连带吞掉转发**？（只报告，**不要改**，我判断）
