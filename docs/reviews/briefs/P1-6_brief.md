# EMlink 任务书 P1-6：单口分流 —— 让 CLI 走 USB 那一个口（跑通阻断项）

## 背景（为什么改）
`usb_cdc.cpp` 现在假设"USB 上有两个虚拟串口"：`Serial`=CDC0（MAVLink）、`Serial1`=CDC1（CLI）。
**这个假设不成立**：本工程 `platformio.ini` 只有 `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`，**没有配置 TinyUSB 复合描述符**（双 CDC 需要它）。ESP32-S3 上 `Serial1` 是 **UART1 硬件串口**（能编译过不代表存在第二个 CDC），而代码从没 `Serial1.begin()`，引脚也没接任何东西 → **CLI 输出进黑洞、输入永远为 0**。

后果（第一天就会撞上）：设角色的唯一入口是 CLI（网页要 WiFi，而 `wifi` 命令本身要 CLI；远程 `cfg` 要有对面 GCS）→ **CLI 不通 = 角色设不了 = 两块板连不起来**。上电日志本身正常（`Serial` = USB 虚拟串口 ✔），所以现象是"**能看到开机日志，但敲 `help` 没反应**"。

修法（审查日志 P1-6 的选项①）：**单口分流** —— 所有 USB 数据走 `Serial` 一个口，在 `usb_task` 里按**字节**区分：
- 遇到 `0xFD`（MAVLink v2）或 `0xFE`（v1）开头 → 按帧收完整帧，投进原来的 `q_usb_rx`
- 其余字节 → 当作 **CLI 文本**，放进一个环形缓冲，由既有的 `usb_cli_read()` 取走

**关键约束：`cli.cpp` 一行都不要改**（它现在是"逐字节读 + 自己拼行 + 回显"）—— 只要 `usb_cli_available/read` 改成从环形缓冲取字节，CLI 的行为完全不变。

## 允许改动的文件（其它一律不碰）
1. `firmware/src/drivers/usb_cdc.cpp`
2. `firmware/src/drivers/usb_cdc.h`（只在需要时加注释/声明）
3. `firmware/src/main.cpp`（见下：USB 任务改为**无条件启动**；删掉 `loop()` 里的配置模式回显）
**不要改 `app/cli.cpp`**；不要改任何 `.md`；不要运行 pio/编译；不要 git commit/add。

## 1. 发送方向：一个互斥锁保护 `Serial` 的写
`Serial` 现在会被两个来源写：`usb_task`（MAVLink 出）和 `cli_print`（CLI 回显）。**不加保护会把 CLI 文本插进 MAVLink 帧中间**，QGC 会解析出错。
- `usb_cdc_init()` 里创建一个递归/普通互斥量：`g_usb_tx_lock = xSemaphoreCreateMutex();`
- 所有 `Serial.write(...)` 调用（`usb_mav_write()` 与 `usb_cli_write()`）都包在
  `xSemaphoreTake(g_usb_tx_lock, portMAX_DELAY)` / `xSemaphoreGive(...)` 之间
- 用互斥量而不是加队列：CLI 需要**立刻回显**（队列做不到同步），而且 MAVLink 侧本来就是 `usb_task` 独占写，加锁只是防 CLI 插队

## 2. 接收方向：`usb_task` 成为 `Serial` 的唯一读者 + 字节分流

在 `usb_cdc.cpp` 里加一个**逐字节状态机**（放在 `usb_task_func` 的读循环里，或抽成 `static void usb_demux_byte(uint8_t b)`）：

```
状态：空闲 / 收帧中（记录 目标帧长、已收字节数、缓冲）
接收到一个字节 b：
  · 若"收帧中"：存入帧缓冲；已收满 → 投 q_usb_rx（沿用现有 queue_item_t 逻辑）→ 回到空闲
  · 若"空闲" 且 b == 0xFD：MAVLink v2 帧头开始（帧长 = 10 + payload_len + 2，payload_len 是紧随其后的第 2 个字节）
  · 若"空闲" 且 b == 0xFE：MAVLink v1（帧长 = 6 + payload_len + 2）
  · 其它情况：**这是 CLI 文本字节** → 推进环形缓冲
```

### 2.1 帧长计算（务必按这个实现）
- v2：`0xFD | len | incompat | compat | seq | sysid | compid | msgid[3] | payload[len] | crc[2] | (可选 13 字节签名)` → 总长 = **10 + len + 2**（暂不含签名：本工程不用 MAVLink 签名，遇到多余字节会在下一轮被当成文本，可接受）
- v1：`0xFE | len | seq | sysid | compid | msgid | payload[len] | crc[2]` → 总长 = **6 + len + 2**
- 帧缓冲上限 280 字节（MAVLink v2 上限 279）；超过就丢弃并复位状态机

### 2.2 卡帧保护（必须有）
若"收帧中"超过 **500 ms** 没有新字节（用 `millis()` 差值判断），**复位状态机**（把已收的半截帧丢弃）。否则一次误判（例如有人在 CLI 里粘了含 `0xFD` 的二进制）会永久卡死接收方向。

### 2.3 CLI 文本环形缓冲
- `static uint8_t g_cli_ring[256]; static volatile uint16_t g_cli_head, g_cli_tail;`（单写者 = usb_task，单读者 = cli_task；满了就丢最新字节并计一个丢弃）
- `usb_cli_available()` → `g_cli_head != g_cli_tail`
- `usb_cli_read(b, n)` → 从环形缓冲搬最多 n 个字节（**语义不变：逐字节读**）
- `usb_cli_write(b, n)` → `Serial.write()`（加 §1 的锁）

## 3. `main.cpp` 的两处改动
1. **USB CDC 任务改为无条件启动**：把
```c
    } else if (g_cfg.role == ROLE_GCS) {
        usb_cdc_init();
        usb_cdc_start();
    }
```
改成：GCS 分支只留注释说明（USB 通路对三种角色都开，因为 **CLI 就在这个口上**），并在第 5 步之后（或第 6 步之前）**无条件**调用：
```c
    usb_cdc_init();
    usb_cdc_start();     // P1-6：CLI 走 USB 这一个口，所以三种角色都要起（否则首次上电没命令可用）
```
2. **删掉 `loop()` 里的配置模式回显块**（`if (g_cfg.role == ROLE_UNSET) { while (Serial.available()) Serial.write(Serial.read()); }`）—— 它会**抢走**本该给 CLI 的字节（那条注释说的"验证 USB CDC 通路"现在由 CLI 自己体现：敲 `help` 有回显就是通路好的）。

## 4. 已知限制（写进代码注释，不用解决）
- 单口分流的固有代价：如果在 CLI 里粘**二进制**（含 `0xFD/0xFE`）可能被误判成帧头 → 由 §2.2 的 500ms 超时自恢复
- 与 QGC 同时用时，CLI 的文本会混进 MAVLink 流（QGC 会忽略非 MAVLink 字节）；反之 QGC 的二进制字节不会被喂给 CLI（分流在字节层完成）

## 自检（报告里回答）
1. 为什么发送方向用**互斥量**而不是再加一条队列？
2. 为什么 `cli.cpp` 不用改？（说明 `usb_cli_read` 的语义保持了"逐字节"）
3. 为什么必须在 `main.cpp` 里把 USB 任务改成**无条件启动**？（提示：首次上电 role 未设时，GCS 分支不会执行）
4. 为什么 §2.2 的 500ms 卡帧保护是必须的？去掉会怎样？
5. 一个 MAVLink 帧的**负载里**出现 `0x0A`（换行）或 `0xFD`，会不会被误当成 CLI 文本 / 新帧头？说明为什么不会。

## 完成后报告（三件套）
1. 改动摘要（逐文件）
2. 自检结论
3. 本次改动不能保证什么（诚实列出，特别是"串口路由只能在真板上验证"）
