# EMlink 任务书 A-7：死代码/未接线清单清理（对应审查日志 P1-12 第 5 条）

## 原则
**能删就删，删不掉就接线**。每个符号都已经查过全工程引用点，按下表办，不要自行扩大或缩小范围。

## 逐项判定表

| 符号 | 现状（已核实） | 怎么办 |
|---|---|---|
| `q_frag` | 只在 `rtos.h`（extern + 注释）、`rtos.cpp`（定义 + `xQueueCreate(Q_FRAG_LEN,...)`）出现，**没有任何生产者/消费者** | **删**：extern 声明、定义、`xQueueCreate`、注释里的说明行；`Q_FRAG_LEN` 若只服务于它，一并删。省 ~1.7KB RAM |
| `mac_should_ack()` | `mac.h` 声明 + `mac.cpp` 定义，**无人调用** | **删**（声明 + 定义） |
| `mav_router_handle_msg()` | `mav_router.h` 声明 + `mav_router.cpp:251` 定义；注释说"mav_router_feed 内部逐条调它"，但实际**无人调用** | **先判断**：看 `mav_router_feed` 里是否内联了同样的"逐条解析并处理"逻辑。① 若是重复实现 → 让 `mav_router_feed` 改为调用 `mav_router_handle_msg`（消除重复，保留函数）；② 若 feed 的逻辑与它不同/不需要 → **删**函数与声明。二选一，报告里说明选了哪个 |
| `ota_handle()` | `ota.h` 声明 + `ota.cpp:305` 定义；`ota_task` 直接调 `g_server->handleClient()` | **删**（声明 + 定义）；`ota.h` 注释里"也可由 CLI 在无任务时单步"一并去掉 |
| `duty_set_max()` | `duty.h` 声明 + `duty.cpp:24` 定义，**无人调用** | **删**（声明 + 定义）；`duty.md` 不用管（我改文档） |
| `route_init()` | `route.h` 声明 + `route.cpp:19` 定义，**无人调用** | **接线**：在 `main.cpp` 的 `setup()` 里、`mesh_start()` 之前调用 |
| `addr_map_init()` | `addr_map.h` 声明 + `addr_map.cpp:12` 定义，**无人调用** | **接线**：同上（`mesh_start()` 之前） |
| `frag_init()` | `frag.h` 声明 + `frag.cpp:29` 定义，**无人调用** | **接线**：同上（`mesh_start()` 之前） |
| `EV_CFG_CHANGED` | `rtos.h:121` 定义；`cli.cpp:102` 在 `save` 后 `xEventGroupSetBits` 过；**无人等待** | **接线**：`main.cpp` 的 `loop()`（或一个轻量任务）里轮询事件组，收到后：① 重新 `nvs_cfg_load(&g_cfg)` 刷新内存镜像；② `Serial.println` 提示"配置已保存：射频/角色类参数需 `reset` 重启生效"。用 `xEventGroupClearBits` 清位，避免重复处理 |
| `beacon_s` | 只有 NVS 读写 + 校验，业务侧没用 | **不要动 ADR 时序**（ADR 的 5s TDR 周期是硬编码的时间锚点，改成可配属于 P1-3 的范围）。本次只在 `cli.cpp` 的 `status` 里把它显示出来（让人看得见），并在报告里说明"未接线" |
| `name` | 只有 NVS 读写 + 校验 | **接线**：`cli.cpp` 的 `status` 里显示节点名（空则显示 `-`） |
| `sysid` | — | 已在 A-2 接线，本次不动 |

## 允许改动的文件
`firmware/src/rtos.h`、`firmware/src/rtos.cpp`、`firmware/src/link/mac.h`、`firmware/src/link/mac.cpp`、`firmware/src/link/duty.h`、`firmware/src/link/duty.cpp`、`firmware/src/service/mav_router.h`、`firmware/src/service/mav_router.cpp`、`firmware/src/service/ota.h`、`firmware/src/service/ota.cpp`、`firmware/src/service/frag.h`、`firmware/src/service/address_map.h`→（实际名 `addr_map.h`）、`firmware/src/service/addr_map.cpp`、`firmware/src/net/route.h`、`firmware/src/net/route.cpp`、`firmware/src/main.cpp`、`firmware/src/app/cli.cpp`

**不要动**：任何 `.md`（我来写）、`webui.h`、`adr.*`、`radio.*`、`nvs_cfg.*`、`frame_codec.*`、`crypto.*`、`stats.*`。

## 特别注意
0. **顺带修一处陈旧注释**：`frag.h`（以及 `rtos.h` 里若还有）写着"分片头在帧内偏移 10~13"——P1-4 后帧头 15 字节，`frag_hdr` 已是 **11~14**，改掉这两处注释（只改注释）。
1. `main.cpp` 里三个 init 要**放在 `mesh_start()` 之前**（顺序：`route_init(); addr_map_init(); frag_init();`），紧挨着现有模块 init 的排版风格。
2. 删 `q_frag` 时**不要**顺手删别的队列（`q_mac_tx` / `q_radio_hi` / `q_radio_lo` / `q_uart_tx` 等仍在用）。
3. 删函数后，**检查是否还有 include 因此变成孤儿**（只报告，不要为删 include 冒险改动别的文件）。
4. `EV_CFG_CHANGED` 的轮询放在 `loop()` 里即可，**不要**新建任务。

## 完成后报告
1. 逐项：删了哪些 / 接了哪些线（原样 vs 新样子）
2. `mav_router_handle_msg` 选了①还是②，为什么
3. 有没有发现清单之外的死代码/未接线项（只报告，不要动）
4. 现在 `q_frag` 删掉后，`rtos.h` 里还有没有指向已删队列的注释
