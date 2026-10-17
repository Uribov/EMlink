# 空中监听 EMlink 的技术档案（RTL-SDR Blog V4 + SDR++ + GNU Radio）

> 状态：🔶 **部分实测**（软件链路已通，硬件待 dongle 到手）。方案与决策见 `../plans/SDR_LORA_MONITOR_PLAN.md`。
> 本文件只放**结论 + 出处**；每条结论都注明是"官方文档/源码"还是"本机实测"。

---

## 0. 人话版

这份档案记的是"监听自己的 LoRa mesh"这条路上，每一个**不能凭记忆断言**的参数和它从哪来：用了哪个工具、哪个版本的库有什么限制、以及我们本机跑出来的结果。以后（人或子 agent）再动这条链路，先看这里，不用重新踩一遍。

---

## 1. 硬件与软件（本机已装，2026-10-17）

| 组件 | 版本/位置 | 出处 |
|---|---|---|
| SDR 硬件 | RTL-SDR Blog **V4**（R828D 调谐器，500 kHz–1.766 GHz，HF 走 28.8 MHz 内置上变频，1 PPM TCXO，无发射） | `rtl-sdr.com/rtl-sdr-blog-v4-dongle-initial-release/` |
| SDR++ | v1.3.0，`/usr/bin/sdrpp`，官方 `sdrpp_ubuntu_noble_amd64.deb`（2 365 994 B，校验一致） | GitHub `AlexandreRouma/SDRPlusPlus` releases（nightly 资产） |
| GNU Radio | 3.10.9.2 + `gnuradio-dev`（apt） | `gnuradio-config-info --version` |
| gr-lora_sdr | 源码编译安装（`~/src/gr-lora_sdr`，装在 `/usr/local/lib/python3.12/dist-packages/gnuradio/lora_sdr`） | `github.com/tapparelj/gr-lora_sdr` |
| RTL-SDR 驱动 | rtl-sdr-blog 分支编译（`/usr/local/lib/librtlsdr.so.0.6git`，工具在 `/usr/local/bin`） | `github.com/rtlsdrblog/rtl-sdr-blog` |
| 另一份驱动 | apt 的 `librtlsdr2 2.0.1`（`/lib/x86_64-linux-gnu/librtlsdr.so.2`） | SDR++ 的 `rtl_sdr_source.so` 链接的是**这一份**（`ldd` 实测），且该库内字符串含 `Found Rafael Micro R828D tuner` → V4 可用 |

⚠️ 两份 librtlsdr 并存是**预期状态**：SDR++（apt 包）走 `.so.2`，命令行工具（`rtl_test`/`rtl_sdr`）走 `/usr/local` 的 `.so.0`。

---

## 2. 关键架构结论（含出处）

| 结论 | 出处 |
|---|---|
| **一块 dongle 不能被两个程序同时打开** | USB 设备独占；下面三条是"想共享"的各条路为何不行/怎么行 |
| `rtl_tcp` **不能**做共享器：一次只服务一个客户端 | `librtlsdr/src/rtl_tcp.c:606` 单 `accept()` 循环 |
| `rtl_mus`（多客户端广播）**已废弃**：`#!/usr/bin/python2`，2018 后无提交 | `github.com/lars18th/rtl_mus`（`rtl_mus.py` 首行、`pushed_at 2018-02-25`） |
| ✅ **正解**：SDR++ 用自带 `network_sink` 把基带 I/Q 推出去，GNU Radio 侧用现成块接收 | SDR++ `sink_modules/network_sink/src/main.cpp` |
| `network_sink` 输出**裸 int16、无任何包头**：`volk_32f_s32f_convert_16i(buf, samples, 32768.0f, n)` 后直接 `conn->write(...)`；立体声为 I/Q 交错 | 同上（`:241-248` 单声道、`:256-258` 立体声） |
| `network_sink` 支持 **TCP 与 UDP** 两种模式 | 同上（`sinkModesTxt = "TCP\0UDP\0"`，`:32`；UDP 走 `net::openUDP`，`:228`） |
| SDR++ **没有 LoRa 解码器**（Decoders 表：ATV/DAB/M17/pager/meteor/weather sat…），故解调必须交给 gr-lora_sdr | SDR++ `readme.md` 的 Decoders 表 |
| gr-lora_sdr 能力：SF 5–12、CR 0–4、隐/显式头、payload 1–255 B、sync word 可选、白化、payload CRC 校验、显式头校验、低数据率优化、软判决 | gr-lora_sdr `readme.md`（Functionalities） |
| gr-lora_sdr 要求 **GNU Radio 3.10** | `CMakeLists.txt:43` `find_package(Gnuradio "3.10" REQUIRED)` |
| librtlsdr 采样率硬限制：**禁 300 001–900 000 Hz**（且 ≤225 000、>3 200 000 也拒绝） | `librtlsdr/src/librtlsdr.c:1100` |
| 915 MHz 波长 32.8 cm（1/4 波长 8.2 cm）；SF7/BW125 符号时间 1.024 ms、SF9 4.096 ms | 本机计算（`c/f`） |

---

## 3. EMlink 空口参数 ↔ 监听侧参数（对齐表）

| 项 | 固件实际值（出处） | 监听侧设成 |
|---|---|---|
| 频点 | `915.0 MHz` 默认（`service/nvs_cfg.cpp:19`） | SDR++ 中心频率 = 你的实际 freq |
| SF / BW | `SF7` / `BW125`（`nvs_cfg.cpp:20,21`） | `sf=7, bw=125000` |
| 编码率 | `cr=5`（=4/5，`nvs_cfg.cpp:22`） | gr-lora_sdr **`cr=1`**（见 §5 实测） |
| Sync 字 | `0x12` 私有（`nvs_cfg.cpp:24`；RadioLib 注 "actually 0x1424"） | `sync_word=0x12` |
| 帧头/CRC | 显式头 + payload CRC 开 + preamble 8（RadioLib `SX126x.cpp:44,47` 默认，固件从不改） | `impl_head=False, has_crc=True, preamb_len=8` |
| 采样率 / 过采样 | — | `1.0 MS/s` → `os_factor=8`（`samp_rate/bw` 必须为整数） |
| 加密 | 默认关（密钥全 0，`service/crypto.md`） | 明文直读 |

---

## 4. 本机环境坑（都踩过，都记下解法）

| 症状 | 根因 | 解法 |
|---|---|---|
| `from gnuradio import gr` → `ImportError: initialization failed` | `/usr/local/lib/python3.12/dist-packages` 里 pip 装的 **numpy 2.4.1** 压过系统 1.26.4，与 GNU Radio 的 ABI 不匹配 | 运行时 `PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages`（**顺序不能反**：系统路径在前，numpy 才取 1.26，而 `gnuradio` 用 `extend_path` 仍能发现 `/usr/local` 下的 `lora_sdr`） |
| gr-lora_sdr 编译报 `'basic_format_string' is not a member of 'fmt'` | CMake 抓到 **miniconda3 的 fmt 12.1.0**（`fmt_DIR=/home/sun/miniconda3/lib/cmake/fmt`），与系统 spdlog 1.12 不兼容 | cmake 时：`-DCMAKE_IGNORE_PREFIX_PATH=/home/sun/miniconda3 -Dfmt_DIR=/usr/lib/x86_64-linux-gnu/cmake/fmt` |
| 同上，另有 `expected primary-expression before '...'` | gr-lora_sdr 未设 C++ 标准 | `-DCMAKE_CXX_STANDARD=17` |
| `make install` 前手动 `rm -rf build` 报"权限不够" | 之前某次 `sudo make install` 在 build 目录留下 root 拥有的文件 | 用 `sudo rm -rf build`，或换新构建目录（本次用 `build2/`） |
| `aria2c`/`apt` 报 GLIBCXX 版本错误 | Hermes 运行时把自己的 `_internal/libstdc++` 塞进 `LD_LIBRARY_PATH` | 命令前 `unset LD_LIBRARY_PATH` |
| GitHub release 下载极慢/断流 | `github.com` 主站 443 卡 | 走 `api.github.com` 拿签名直链 + `aria2c -x16 -s16`（实测 3 MB/s） |
| 从 Hermes 启动 GUI 报 `libvte-2.91.so.0: GLIBCXX_3.4.31 not found`（gnome-terminal 起不来） | 子进程继承 Hermes 的 `LD_LIBRARY_PATH`（旧 libstdc++） | 在**启动器那一层** `env -u LD_LIBRARY_PATH`；GRC 已用 `/usr/local/bin/gnuradio-companion` 包装修好（同一原因：numpy 2.4.1 冲突），撤销 `sudo rm` |

---

## 5. 实测记录

### P0 软件自环（`tx_rx_functionality_check.py`，无需任何硬件）—— ✅ 通过（2026-10-17）

命令（关键环境变量）：
```bash
unset LD_LIBRARY_PATH
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
cd ~/src/gr-lora_sdr/examples && ( sleep 25; echo ) | python3 tx_rx_functionality_check.py
```
该脚本参数：`bw=125000, sf=7, cr=1, sync_word=0x12, impl_head=False, has_crc=True, preamb_len=8, samp_rate=bw*4`。

实测输出（连续多帧）：
```
--------Header--------
Payload length: 14
CRC presence:   1
Coding rate:    1
Header checksum valid!

rx msg: Hello world: 0
CRC valid!
```

**结论**：
1. GNU Radio + gr-lora_sdr 工具链完好（发→收闭环，payload 无损）；
2. **CR 编号映射确认**：gr-lora_sdr 的 `cr=1` 在空口就是 4/5 → 等于固件的 `cr=5`；监听侧一律填 `cr=1`；
3. 显式头（`Payload length=14`）与 payload CRC（`CRC valid!`）都被正确识别与校验，与固件 RadioLib 默认设置一致；
4. 该脚本**无限循环**且以 `input('Press Enter to quit')` 结束 → 自动化时必须喂 stdin（本机用 `( sleep N; echo ) |`），否则会瞬间退出、看不到任何帧。

### S2 离线端到端（造 IQ → 解调 → 解帧 → MAVLink 打印）—— ✅ 通过（2026-10-17）

产物：`tools/sdr/make_test_iq.py`（发射链造 IQ）、`tools/sdr/lora_mesh_live.py`（接收流图 + 解析 + 打印）；自测命令见 `tools/sdr/README.md` §3。

实测结论（可复现）：

| 结论 | 证据 |
|---|---|
| **1.0 MS/s（`os_factor=8`）可用**，不必退回 `bw*4` | 造 IQ 与解码全程用 `samp_rate=1e6`；`lora_mesh_live.py` 报 `os_factor=8` 并把 4 条帧全解出 |
| 基带 int16 交错 → complex 的正确缩放是 `1/32768` | `complex_to_interleaved_short(32768)`（TX）/ `interleaved_short_to_complex(1/32768)`（RX）闭环无损 |
| `packet_len` tag 流模式可用（一条 tag = 一条 LoRa packet = 一条 mesh 帧） | `whitening(False, True, ',', 'packet_len')` + `vector_source_b(..., tags)`；实测 packet_len = 38/57/192/42，恰为 `15+payload+2` |
| 接收侧拿 payload 的 tag：key = `frame_info`，dict 含 `pay_len / crc_valid / crc / err / ldro_mode / cr` | 实测 dict：`((crc_valid . #t) (err . 0) (ldro_mode . 2) (crc . 1) (pay_len . 38) (cr . 1))`；tag offset 0/38/95/287。出处 `lib/header_decoder_impl.cc:59-68`、`lib/crc_verif_impl.cc:150-155` |
| TX `interleaver(cr, sf, ldro=0, bw)`；RX `header_decoder(..., pay_len=255, has_crc, 2, True)` 与之配合正常 | 链路解出全部帧；tag 里 `ldro_mode=2` 不影响 |
| 分片两片各自能过 LoRa payload CRC，**仍需应用层重组** | 两片分别解出，`Reassembler` 拼成 200 B `LOGGING_DATA` |
| 发送速率过快时本地 UDP 回环丢首帧（`Insufficient block data`） | 实测观察；LoRa 每帧重新同步，只影响当前帧 |

### 待实测（dongle 到手后）

| 项 | 怎么测 | 期望 |
|---|---|---|
| V1 驱动认 V4 | `rtl_test`（/usr/local 版） | 认出 `Rafael Micro R828D tuner` |
| V2 看见信号 | SDR++：915.000 MHz、手动增益、瀑布时间跨度调短 | 每秒一条 BEACON 细亮线（`beacon_s=1`）；"白墙"= 饱和 |
| V3 裸流格式与字节率 | SDR++ 开 `network_sink`（UDP，1.0 MS/s，立体声 I/Q），另开终端收 2 秒 | ≈ 4 MB/s（=1.0 MS/s × 2 ch × 2 B） |
| V4 中心频率语义 | 看 `network_sink` 输出是"源中心频率"还是"VFO 频移后"零中频 | 决定中心频率怎么设（SDR++ 里 VFO 是否需归零） |
| V5 采样率可用性 | SDR++ 源采样率菜单能否给 1.0 MS/s | 给不了就退 2.0 MS/s（`os=16`）或插重采样 |
| V6 端到端 | 抓真实帧 → 解析 15 B 头 + CRC16 → 与固件日志逐字节对照 | 一致 |
| V7 MAVLink | pymavlink 解码打印 | 出 `HEARTBEAT` 等消息名 |
