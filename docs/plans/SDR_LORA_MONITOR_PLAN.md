# SDR 监听方案：用 RTL-SDR Blog V4 + SDR++ 解 EMlink 空口 MAVLink2

> 状态：⬜ **未实施**（方案稿，**未提交**）。等技术事实与决策点（§8）拍板后再动手装/写代码。
> 关联：`docs/references/LLCC68_E220-900M22S.md`（射频参数出处）、`firmware/src/link/frame_codec.h`（帧格式权威）、`firmware/src/service/mav_router.md` / `crypto.md` / `frag.md`。
> 本文只记录**已查实的技术事实 + 出处**；推测项一律标「待实测」。

---

## 0. 人话版

无线电是"广播"的：915MHz 上谁说话，旁边任何人都能听。所以监听自己的 LoRa mesh **不用改固件、不用插飞控**，只要一只"只收不发的耳朵"。

RTL-SDR V4 就是这只耳朵——它把 915MHz 附近的电磁波录成数字样本。但录下来的是"波形"，要变成能读的东西得翻译三层：

1. **波形 → 字节**：靠软件 `gr-lora_sdr` 当 LoRa 解调器；
2. **字节 → Mesh 帧**：靠自己写的解析器（规则与固件 `frame_codec` 完全一致）；
3. **Mesh 帧 → MAVLink**：靠现成的 `pymavlink` 翻译成人能读的消息名 + 字段。

**"边看边解"怎么做到**（用户明确要求）：SDR++ 窗口看频谱瀑布，另一个终端窗口看解码结果。
难点是"一块 dongle 不能被两个程序同时打开"。已查实的解法：**SDR++ 独占 dongle，然后用它自带的 `network_sink` 把基带 I/Q 以裸数据流推给 GNU Radio**——因为是无封装的 int16 流，GNU Radio 用现成积木块就能接，**不需要写任何自定义块**。

---

## 1. 目标形态（两个窗口）

| 窗口 | 程序 | 干什么 |
|---|---|---|
| **A（图形）** | SDR++ | 看频谱/瀑布：信号在不在发、每秒几条、强度如何；顺带把最佳增益/衰减调出来 |
| **B（终端）** | GNU Radio 脚本 + 解析器 | 实时打印：EMlink 帧头字段（src/dst/seq/ttl/orig_sysid/frag）+ MAVLink 消息名与字段 |

SDR++ **不解 LoRa**（官方模块清单的 Decoders 表里没有 LoRa 解码器），所以它只当"眼睛"；解码全部在窗口 B。**但它的 `network_sink` 能把 I/Q 流出来**，这就是"边看边解"的桥。

---

## 2. 为什么一块 dongle 能同时给两个程序用（已查实）

| 事实 | 证据 |
|---|---|
| dongle 是 USB 独占设备，两个程序不能各自打开 | 常识；下面两条是"想绕过它"的路子为何不成立 |
| `rtl_tcp` **不能**当共享器：它一次只服务一个客户端 | `librtlsdr/src/rtl_tcp.c:606` 的 `accept(listensocket,...)` 单连接循环 |
| `rtl_mus`（多客户端广播脚本）**已废弃**：`#!/usr/bin/python2`、2018 年后无提交 | `github.com/lars18th/rtl_mus`（`rtl_mus.py` 首行、仓库 `pushed_at 2018-02-25`） |
| ✅ **正解：SDR++ 自己做源**，把基带 I/Q 从网络推出去 | SDR++ `sink_modules/network_sink/src/main.cpp` |

`network_sink` 源码要点（这就是为什么它能被 GNU Radio 直接吃）：

- 支持 **TCP 与 UDP** 两种模式：`sinkModesTxt = "TCP\0UDP\0"`（`main.cpp:32`）；UDP 走 `net::openUDP("0.0.0.0", port, hostname, port, ...)`（`:228`）
- **输出是裸 int16 样本，没有任何包头/协议封装**：
  - 单声道：`volk_32f_s32f_convert_16i(netBuf, samples, 32768.0f, count)` → `conn->write(count * sizeof(int16_t), netBuf)`（`:241-248`）
  - 立体声（I/Q 交错）：同样转换后 `conn->write(count * 2 * sizeof(int16_t), ...)`（`:256-258`）
- 端口与采样率在 UI 里可配（代码默认值出现 `7355` 与 `4242` 两处，以界面实际显示为准）；sink 的采样率菜单由源的可用采样率生成

配套的 GNU Radio 现成积木块（**零自定义代码**）：

```
blocks.udp_source(type="short", vlen=2)            # 收裸 int16 交错流
  → blocks.interleaved_short_to_complex(scale=1/32768)   # → complex float
  → gr-lora_sdr 接收链
```

> 待实测：`network_sink` 输出的基带是"源中心频率"还是"VFO 频移后"的零中频。若为后者则更省事（SDR++ 里 VFO 设 915.000 MHz 即可）；若是前者，则把源中心频率对准 915.000 MHz 并关闭 VFO 频移。**不影响链路结构，只影响中心频率怎么设。**

---

## 3. 数据流（完整链路）

```
天线 ─► RTL-SDR Blog V4 (R828D)
           └─ SDR++（独占 dongle；915.000 MHz；1.0 MS/s；手动增益；看瀑布）
                 └─ network_sink  UDP → 127.0.0.1:4242   裸 int16 交错 I/Q 流
                       └─ GNU Radio 脚本【终端窗口 B】
                             udp_source(short,vlen=2) → interleaved_short_to_complex(1/32768)
                             → gr-lora_sdr: frame_sync → fft_demod → gray_mapping
                                            → deinterleaver → hamming_dec → header_decoder
                                            → dewhitening → crc_verif
                             → LoRa payload（= 一条 EMlink mesh 帧）
                                   → emlink_frame.py：拆 15B 头 + CRC16-CCITT + 分片重组
                                         → mavlink_print.py：pymavlink 解码打印（可选转发 QGC）
```

---

## 4. 参数对齐表（固件实际值 ↔ 监听侧）

| 项 | 固件实际值 | 监听侧设成 | 出处 |
|---|---|---|---|
| 频点 | `915.0 MHz`（默认，NVS 可改） | SDR++ 中心频率 = 你的实际 freq | `service/nvs_cfg.cpp:19` |
| 扩频因子 | `SF7`（ADR 会在 SF7/SF9 间切） | `sf=7`（ADR 实验期建议关掉） | `nvs_cfg.cpp:20`；`link/adr.md` |
| 带宽 | `BW125 kHz` | `bw=125000` | `nvs_cfg.cpp:21` |
| 编码率 | `cr=5`（即 4/5） | gr-lora_sdr 的 `cr=1`（**编号语义待实测**） | `nvs_cfg.cpp:22` vs gr-lora_sdr 官方例子 `lora_RX.py:42` |
| Sync 字 | `0x12`（私有） | `sync_word=0x12` | `nvs_cfg.cpp:24`；RadioLib `SX126x.h:383` 注 "actually 0x1424" |
| 帧头/E2E CRC | **显式头 + payload CRC 开** + preamble 8 符号 | `impl_head=False, has_crc=True, preamb_len=8` | RadioLib `SX126x.cpp:44,47` 默认；固件从未改这两项 |
| 采样率 | — | **1.0 MS/s**（让 `samp_rate/bw` 为整数 = 8） | 采样率限制见 `librtlsdr/src/librtlsdr.c:1100`（禁 300k–900k） |
| 过采样比 | — | `os_factor = samp_rate/bw = 8` | gr-lora_sdr `frame_sync(...)` 第 6 参 |
| 加密 | 默认**关**（密钥全 0） | 直接看明文 | `service/crypto.md` §状态 |

**顺带的好运**：固件的 LoRa 参数（915MHz / SF7 / BW125 / CR4-5 / sync 0x12 / 显式头 / CRC）**正好等于 gr-lora_sdr 官方示例的默认值**（`examples/lora_RX.py`：`sf=7, bw=125000, cr=1, center_freq=868.1e6`；`examples/tx_rx_functionality_check.py`：`sync_word=0x12, impl_head=False, has_crc=True, preamb_len=8, samp_rate=bw*4`）。唯一需要实测确认的是 **CR 编号**。

---

## 5. 安装（需要 sudo；由用户执行或明确授权后执行）

| 步骤 | 内容 | 备注 |
|---|---|---|
| 1 | RTL-SDR 驱动：`rtl-sdr-blog`（V4 的 R828D 需要该分支/新版上游） | 官方：`github.com/rtlsdrblog/rtl-sdr-blog`；V4 规格见 `rtl-sdr.com/rtl-sdr-blog-v4-dongle-initial-release/` |
| 2 | SDR++：官方 releases 里的 `sdrpp_ubuntu_noble_amd64.deb` | Mint 22.3 = noble 基座；官方 README 明确警告**不要用发行版仓库的 `sdrpp` 包** |
| 3 | GNU Radio：`apt install gnuradio gnuradio-dev`（Ubuntu 24.04 上是 3.10.x） | gr-lora_sdr 要求 GNU Radio 3.10 |
| 4 | `gr-lora_sdr`：源码 cmake 编译（备选：conda 包 `conda install -c tapparelj -c conda-forge gnuradio-lora_sdr`） | 官方安装说明见其 README |
| 5 | 包装脚本 `tools/sdr/sdrpp.sh`：**先 `unset LD_LIBRARY_PATH` 再启动 SDR++** | 本机老坑：Hermes 会把自己的 `_internal` 旧库塞进该变量，导致系统 GUI 程序起不来 |

在线解码用的 Python 包：`pymavlink` 本机已装 ✅。

---

## 6. 验证步骤（现象驱动，一步一判据）

| # | 做什么 | 判据 |
|---|---|---|
| V1 | `rtl_test` | 认出设备与 **R828D** 调谐器 |
| V2 | SDR++ 看谱（915.000 MHz、手动增益、瀑布时间跨度调短） | 见 **每秒一条细亮线**（BEACON `beacon_s=1`）；若整片"白墙"= 前端饱和 → 降增益 / 降 EMlink 功率 / 拉远或加衰减 |
| V3 | SDR++ 开 `network_sink`（UDP，1.0 MS/s，立体声 I/Q），另开终端收 2 秒并统计字节率 | 字节率 ≈ 采样率 × 4 B/s（1.0 MS/s → ≈4 MB/s）→ 说明"裸 int16 交错"判定正确 |
| V4 | GNU Radio 流图先只做"收流 + 画谱/存文件" | IQ 能连续收到、不丢爆 |
| V5 | 接 gr-lora_sdr（先用官方自环程序 `tx_rx_functionality_check.py` 验工具链与 CR 编号） | 自环能出 payload ⇒ 工具链正确 |
| V6 | 接真实信号 → 帧解析 + CRC16 校验 | 抓到的 15 字节头与固件日志逐字节一致、CRC 通过 |
| V7 | MAVLink 打印 | 按名字打出 `HEARTBEAT / SYS_STATUS / GPS_RAW_INT / ATTITUDE` 等 |

---

## 7. 已知会咬人的坑

1. **采样率必须让 `samp_rate/bw` 是整数**（`frame_sync` 的 `os_factor` 是整数）：1.0 MS/s → `os=8` ✅；SDR++ 菜单常见的 **1.024 MS/s 会得 8.192 ✗**，要么改用 2.0 MS/s（`os=16`），要么中间插 rational resampler。**这是本方案第一个要实测的点。**
2. **SDR++ 源采样率菜单可能没有 1.0 MS/s**（常见是 1.024/2.048…）→ 走上面备选。
3. **近距一定饱和**：EMlink 默认 20 dBm；1 米内会灌爆 8-bit ADC → 实验期降到 ≤0 dBm，或拉开 3–5 m，或加 20–30 dB 衰减。
4. **UDP 本地回环偶发丢包**：不致命（LoRa 每帧重新同步），若影响解码则改 TCP（需一个十几行的 Python 桥转 UDP/ZMQ）。
5. **CR 编号语义**待实测：固件 `cr=5` 表示 4/5；gr-lora_sdr 例子里写 `cr=1`。先用官方自环确认它的语义。
6. **ADR 会切 SF7↔SF9**：一块 dongle 一个参数集 → 实验期关 ADR，或后面开两路解码。
7. **带宽若改 250/500 kHz**：1.0 MS/s 对 BW250 是 `os=4` ✅；BW500 需 ≥2 MS/s（超过 3.2 MS/s dongle 不接受）。
8. **加密**：默认关；开了加密（密钥非全 0）时空中是密文，工具需加一路 AES-128-CCM 解密（nonce 规则见 `crypto.md`）。

---

## 8. 决策点（拍板后我才动手）

| ID | 决策 | 建议 |
|---|---|---|
| D1 | 采样率路线：1.0 MS/s vs 2.0 MS/s(os=16) vs 加重采样 | 先试 1.0 MS/s，不行退 2.0 MS/s |
| D2 | `network_sink` 用 **UDP**（零胶水）还是 TCP（需小桥） | UDP 先跑通，丢包严重再换 |
| D3 | 窗口 B 形态：**纯 Python 脚本 headless**（好进终端/日志）vs `.grc` 流图 | Python 脚本 |
| D4 | 是否同时保留"录 IQ 离线复跑"通道 | 保留（可复现、可留档、便于对照固件日志） |
| D5 | 现在开始装（需你 sudo）+ 写脚本（交 OpenCode）？ | 等 D1–D4 |

## 9. 产物落点（实施时）

- `tools/sdr/sdrpp.sh`（带 `unset LD_LIBRARY_PATH` 的启动包装）
- `tools/sdr/lora_mesh_live.py`（GNU Radio 流图：收流 → 解调 → 打印）
- `tools/sdr/emlink_frame.py`（帧解析 + CRC16 + 分片重组，常量对齐 `frame_codec.h`）
- `tools/sdr/mavlink_print.py`（pymavlink 解码 + 可选转发）
- `tools/sdr/README.md`（参数表、复现步骤、实测结论）
- `docs/references/SDR_LORA_MONITOR.md`（硬件/工具规格与出处留档）
- 任务书走 `docs/reviews/briefs/`，用完随提交 `git rm`
