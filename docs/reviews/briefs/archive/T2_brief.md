# EMlink 任务书 T-2：协议版本宣告（三件最疼之一：拦"改了帧头两端悄悄不兼容"）

## 背景（为什么改）
A-5 那批改动把帧头从 14 字节改成 15 字节（加 `enc_epoch`）——**帧格式不兼容**，但两端跑不同版本时**没有任何提示**：老固件解新帧时字段错位、CRC 校验直接失败、帧被静静丢掉，现场只表现为"怎么都通不上"，很难判断到底是版本问题还是射频问题。

修法：**在 BEACON 里带 1 字节协议版本**（BEACON 是每个周期都广播、所有节点都会收到的"广告位"），收端与自己比对，不一致就**计数 + 串口告警 + CLI 显示**（只告警不拒绝——还要能互相看见，便于现场判断该刷哪一台）。

**诚实说明（必须写进代码注释）**：这套机制只能拦住**未来**的版本不一致（比如 v2↔v3）。对已经发生的 14/15 字节不兼容（v1↔v2）无能为力——旧固件连帧都解不出来，根本走不到"读 payload 里的版本号"这一步。

## 允许改动的文件（其它一律不碰）
1. **新建** `firmware/src/service/version.h`
2. **新建** `firmware/src/service/version.cpp`
3. `firmware/src/net/mesh.cpp`
4. `firmware/src/net/mesh.h`
5. `firmware/src/app/cli.cpp`
不要改任何 `.md`；不要运行 pio/编译；不要 git commit/add。

## 1. 新建 `service/version.h`
```c
#pragma once
#include <Arduino.h>

// ---- 版本 ----
// 固件版本：手维护（改功能时手工 +0.0.1；自动从 git 生成版本号的办法记在 ROADMAP.md，现阶段不做）
#define EM_FW_VERSION      "0.1.0"

// 协议版本（空口帧格式 / 封装语义的兼容性编号）：
//   v1 = 14 字节帧头（A-5 之前）
//   v2 = 15 字节帧头（新增 enc_epoch）+ AEAD(AAD/nonce/replay window)
// ★ 凡是改动帧格式、字段含义、封装流程，必须在这里 +1；两端不一致时的表现见 version.cpp 的说明。
#define EM_PROTO_VERSION   2

// 固件版本字符串（"0.1.0"）、协议版本字符串（"v2"）
const char* version_fw_str(void);
const char* version_proto_str(void);
```

## 2. 新建 `service/version.cpp`
实现两个返回字符串的函数（把 `EM_FW_VERSION` 与 `"v"` + `EM_PROTO_VERSION` 拼出来，用 `static char` 缓冲或直接返回字面量，别用动态内存）。
文件顶部写一段注释说明协议版本的作用与"只能拦未来的不一致"这个限制（照抄背景里的诚实说明）。

## 3. `net/mesh.cpp` / `mesh.h`

### 3.1 发：BEACON 带 1 字节协议版本
`mesh_send_beacon()` 现在是**空 payload**（`payload_len = 0`）。改成 1 字节 payload = `EM_PROTO_VERSION`：
- 用一个该函数内的 `static`/局部 `uint8_t ver = EM_PROTO_VERSION;` 作为 payload 指针（注意 payload 必须在 `frame_encode` 拷贝前一直有效——参考 `crypto_seal_frame` 那处的注释）
- BEACON **不加密**，这一点不变（保持原样）

### 3.2 收：比对 + 计数 + 告警
在 `mesh_task` 收到 `FRAME_BEACON` 的分支里（现在调 `route_learn` / `route_update_rssi` / `adr_on_beacon` 那里）追加：
- `payload_len >= 1` → `peer_ver = f.payload[0]`
- `payload_len == 0` → `peer_ver = 0`（老固件/无版本）
- 若 `peer_ver != 0 && peer_ver != EM_PROTO_VERSION`：`g_proto_mismatch++`；并且**每个对端版本只打印一次**串口告警（用一个 `static uint8_t warned_ver = 0;` 记住已警告过的版本号，避免每周期刷屏）：
  ```c
  Serial.printf("[mesh] 协议版本不一致：本机 v%u，对端 v%u（帧格式/封装不兼容，需全网同版本固件）\n",
                (unsigned)EM_PROTO_VERSION, (unsigned)peer_ver);
  ```
- 记录"最近一次见到的对端协议版本"到 `static uint8_t g_peer_proto = 0;`

### 3.3 `mesh.h` 新增诊断接口
```c
uint32_t mesh_proto_mismatch_count(void);   // 协议版本不一致的信标计数（T-2）
uint8_t  mesh_peer_proto_version(void);     // 最近一次收到的对端协议版本（0 = 未收到/老版本无此字段）
```

## 4. `app/cli.cpp`
1. 新增 `ver` 命令：打印固件版本 + 协议版本（例：`EMlink 固件 0.1.0 · 协议 v2`）
2. `status` 增加一行（**十三行**），放在 ADR 行之后：
```c
        snprintf(line, sizeof(line), "协议: 本机=%s 对端=%s 不一致=%u 次\r\n",
                 version_proto_str(), mesh_peer_proto_version() ? "<见下>" : "未知",
                 (unsigned)mesh_proto_mismatch_count());
```
（"对端"那格：`mesh_peer_proto_version()` 为 0 时显示 `未知`，非 0 时显示 `v<N>`；实现时用一个小 `char` 缓冲拼，别写成上面的占位符）
3. 需要时补 include（`../service/version.h`）

## 自检（报告里回答）
1. 为什么把协议版本放在 BEACON，而不是每一帧都带、也不是只在 OTA 页面显示？
2. 为什么**只告警不拒绝**（不做"版本不一致就丢包"）？
3. 为什么这套机制拦不住历史上那次 14→15 字节的不兼容？（提示：旧固件连解帧都失败，走不到读 payload）
4. BEACON 改成 1 字节 payload 后，**旧固件**收到会怎样？（提示：`frame_decode` 里的长度校验与 payload_len 字段）

## 完成后报告（三件套）
1. 改动摘要（逐个文件：原样 → 新样子）
2. 自检结论
3. 本次改动不能保证什么（诚实列出）
