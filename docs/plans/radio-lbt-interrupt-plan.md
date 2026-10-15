# radio-update —— 未来更新方向（不在当前实现范围）

> 本文是**未来演进方案留档**，不是当前实现的说明；当前实现见 `radio.md`。
> 约定：只记录"已评估、暂不实施"的方案，写清 动机 / 做法 / 收益 / 代价 / 落地步骤 / 触发条件。
>
> ### ⬜ 状态：**未实施**（本文是"以后可以做"的方案，**不是当前实现的说明**）
> 触发条件见文末 §触发条件；**当前实现**（状态驱动轮询，P0-3）见 `../../firmware/src/drivers/radio.md` 的 §关键实现点 4。

---

## 更新 1：中断驱动的 CAD（信道侦听）—— 替换"状态驱动轮询"

### 背景：现状是什么
P0-3 已把 CAD 修成**状态驱动轮询**：`startChannelScan()` 启动 → 循环读 `getChannelScanResult()`
直到 `CAD_DONE`（空闲）/ `CAD_DETECTED`（忙），超时按 SF/BW 推导（5 个符号 + 3ms 余量），
超时/未知一律按"忙"处理（宁可退避不误发）。

它**正确**，但有两个固有代价：
1. 轮询期间 `mac_task` 在 `vTaskDelay(1)` 上空转约 5~25ms（SF7 → SF9 慢速窗），短帧高吞吐时摊薄信道利用率；
2. 极端干扰下的"超时兜底"会按"忙"处理，产生不必要的退避（保守但费时）。

### 方案（中断驱动）
让 CAD 完成也走 DIO1 中断，与收包共用一套事件机制：
- `radio_cad_start()`：启动 CAD 立即返回；
- `radio_isr`（DIO1，已有）**不读 SPI**，只置一个 `volatile` 事件标记 + 通知 `rf_task`；
- `rf_task` 统一分发：`RX_DONE` → 读包投队列；`CAD_DONE/CAD_DETECTED` → 记录结果并通知等待者；
- `radio_cad_wait(timeout_ms, int16_t* result)`：`ulTaskNotifyTake` 阻塞等结果（等待期间 RTOS 跑别的任务）；
- `mac_lbt()` 改成"start + wait"两段式。

### 预期收益
- LBT 等待期间不再忙等（CPU 占用下降，语义更干净）；
- 所有射频事件统一由中断驱动，`radio` 内的状态机只有一处（rf_task）。

### 代价 / 风险
- **ISR 里不能读 IRQ 状态（SPI）**：必须把"判断事件类型（RX_DONE vs CAD_DONE）"挪到任务上下文——
  即 ISR 只发"有事件"通知，rf_task 醒来后读 IRQ 状态再分发；
- DIO1 的 IRQ 掩码在"收包"与"CAD"两种状态下不同（RadioLib 的 `startReceive` 与 `startChannelScan`
  各自设置掩码）→ 两条路径都要显式管理事件类型，别把对方的事件吃错；
- `mac_send` 的调用时序从"同步轮询"改"两段式"，改动面比现在大；
- 通知只有一槽（计数锁存），要和 `rf_task` 自己等 DIO1 的用法区分开（等待者不是 rf_task，是本机调用者）。

### 落地步骤（将来做时）
1. `radio.h` 增加 `radio_cad_start()` / `radio_cad_wait(timeout_ms, int16_t* result)`；
2. `radio_isr` 内不读 SPI，只置事件标记 + `vTaskNotifyGiveFromISR(g_rf_task)`；
3. `rf_task` 读 IRQ 状态后分发：RX_DONE → `radio_readPacket` 投 `q_rf_rx`；CAD 完成 → 唤醒等待者；
4. `mac_lbt()` 改为 start + wait（保持"忙/错误 → 退避重试"的语义不变）；
5. 同步更新 `radio.md` / `mac.md`。

### 触发条件（什么时候值得做）
- 实测吞吐受 LBT 轮询影响明显（例如 SF7 小帧连续发送、mac_task 空转比例高）；
- 或需要更低功耗（电池节点、占空比紧的部署）。
