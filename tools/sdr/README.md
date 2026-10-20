# tools/sdr —— 空中监听 EMlink（RTL-SDR + SDR++ + GNU Radio）

监听自己开发的 LoRa Mesh 数据链：不改固件、不插飞控，从空中把 MAVLink 消息读出来。
技术档案（参数出处、环境坑、实测记录）：`docs/references/SDR_LORA_MONITOR.md`；方案与决策：`docs/plans/SDR_LORA_MONITOR_PLAN.md`。

## 0. 这条链路长什么样

```
天线 ─► RTL-SDR Blog V4 ─► SDR++（看频谱/瀑布，独占 dongle）
                              └─ network_sink  UDP → 127.0.0.1:4242   裸 int16 交错 I/Q
                                    └─ lora_mesh_live.py【终端窗口】
                                          int16 → complex → gr-lora_sdr 解调 → mesh 帧
                                          → 帧头解析 + CRC16 + 分片重组 → pymavlink 打印
```

SDR++ 不解 LoRa（官方无 LoRa 解码器），它只负责「看得见」+ 把基带 I/Q 推出来；解码全在终端窗口。

两个窗口分工：一个看有没有信号，一个看信号里是什么。

## 1. 文件清单

| 文件 | 职责 | 需要 gnuradio? |
|---|---|---|
| `emlink_frame.py` | Mesh 帧编解码、CRC16-CCITT、分片重组（规则与固件 `frame_codec`/`frag` 一致） | 否 |
| `mavlink_print.py` | MAVLink 2 解码与一行格式化（v20 方言） | 否（需 pymavlink） |
| `make_test_iq.py` | 用 gr-lora_sdr 发射链造一段含 Mesh 帧的 IQ（离线自测用） | 是 |
| `lora_mesh_live.py` | 接收流图：文件或 UDP 输入 → 解调 → 解帧 → 打印 | 是 |

前两个是纯解析、可单独跑；后两个依赖 GNU Radio。

## 2. 运行环境（必读，否则大概率 import 失败）

```bash
unset LD_LIBRARY_PATH
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
```

- 必须用 `/usr/bin/python3`（本机默认 `python3` 可能是 miniconda 3.14，没有 gnuradio/pymavlink）。
- `PYTHONPATH` 两条顺序不能反：系统路径在后会让 `/usr/local` 的 numpy 2.4.1 顶掉系统 1.26.4 → `from gnuradio import gr` 报 `initialization failed`。
- 纯解析模块（`emlink_frame.py` / `mavlink_print.py`）不需要 gnuradio，普通 `python3` 也能跑自测。

环境变量这两行是这条链路最容易出错的地方，跑任何脚本前先设。

## 3. 离线自测（不需要任何 SDR 硬件）

```bash
unset LD_LIBRARY_PATH
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
/usr/bin/python3 tools/sdr/make_test_iq.py --out /tmp/test.i16       # 造 4 条帧的 IQ
/usr/bin/python3 tools/sdr/lora_mesh_live.py --file /tmp/test.i16 --limit 4
```

期望看到（节选）：

```
DATA src=3 dst=0 relay=2 ttl=4 sysid=1 net=0 proto=MAVLINK qos=1 ack=0 frag=0 enc=0 epoch=7 seq=42 len=21
  └ HEARTBEAT sys=1 comp=1 type=2 autopilot=12 base_mode=0 custom_mode=0 system_status=3 mavlink_version=3
DATA ... seq=43 len=40
  └ GLOBAL_POSITION_INT sys=1 comp=1 time_boot_ms=1000 lat=39.9 lon=116.4 alt=120 ...
DATA ... frag=1 ... frag=9/0/2/123
  └ [分片] 1/2 已收 175 字节
[重组] 完整消息 200 字节
  └ LOGGING_DATA sys=1 comp=1 target_system=1 target_component=1 sequence=7 length=181 ...
统计: 帧数=4 解帧失败=0 MAVLink消息=3 重组完成=1 重组超时=0
```

各自的单元自测：`/usr/bin/python3 tools/sdr/emlink_frame.py --selftest`（7 项）、`... mavlink_print.py --selftest`（6 项）。

## 4. 在线监听（dongle 到手后）

### 4.0 两个窗口怎么开（边看边解）

关键前提：从 Hermes（或任何带 `LD_LIBRARY_PATH` 污染的环境）启动 GUI，必须在启动器那一层清掉它 —— 否则 `gnome-terminal` 自己会因 libvte 报 `GLIBCXX_3.4.31 not found`，SDR++ 也可能直接崩。

```bash
# 窗口 1：SDR++（独占 dongle，看频谱/瀑布）
env -u LD_LIBRARY_PATH sdrpp

# 窗口 2：解码终端（自动设好 PYTHONPATH，跑在线解码）
env -u LD_LIBRARY_PATH gnome-terminal --window --title="EMlink 监听 (UDP 4242)" -- bash -c '
  export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
  cd ~/EMlink
  /usr/bin/python3 tools/sdr/lora_mesh_live.py --udp 4242 --hex
  exec bash'
```

- 纯终端里跑（不要新窗口）：`unset LD_LIBRARY_PATH; export PYTHONPATH=...`（见 §2）。
- GNU Radio Companion：本机已加入口修正 `/usr/local/bin/gnuradio-companion`（同样是 numpy 冲突），菜单和终端都能直接开；撤销 `sudo rm /usr/local/bin/gnuradio-companion`。
- 确认窗口真的起来了：`wmctrl -l | grep -iE "gnu radio|EMlink"`。

### 4.1 SDR++ 配置

1. SDR++（启动前先 `unset LD_LIBRARY_PATH`）：
   - Source `RTL-SDR`；中心频率 = 你固件里的 freq（默认 `915.000 MHz`）；
   - 采样率 1.0 MS/s；增益手动（不要 auto）；
   - 瀑布时间跨度调短 —— 一帧只有几十 ms，是细竖线；BEACON 每秒一条（`beacon_s=1`）；
   - 开 `network_sink`：模式 UDP、端口 4242、采样率 1.0 MS/s、立体声（I/Q）。
2. 终端：
   ```bash
   unset LD_LIBRARY_PATH
   export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
   /usr/bin/python3 tools/sdr/lora_mesh_live.py --udp 4242 --hex
   ```

若 SDR++ 菜单里没有 1.0 MS/s：改用 2.0 MS/s 并把脚本 `--samp-rate 2000000`（`os_factor` 会算成 16）。

## 5. 空口参数（必须与固件一致；改固件 NVS 后要同步这里）

| 项 | 值 | 出处 |
|---|---|---|
| 采样率 / os_factor | 1.0 MS/s → 8 | librtlsdr 禁 300k–900k（`references` 档案） |
| SF / BW | 7 / 125 kHz | `firmware/src/service/nvs_cfg.cpp` |
| CR | `cr=1`（= 空口 4/5 = 固件的 `cr=5`） | 已实测确认（档案 §5） |
| sync word | `0x12` | 同上 |
| 头 / CRC / preamble | 显式头 / CRC 开 / 8 符号 | RadioLib 默认，固件未改 |
| ldro | 0（SF7/BW125 符号时间 1.024 ms，不需要） | 本机计算 |

这张表改动时要和 `references/SDR_LORA_MONITOR.md` §3 一起改。

## 6. 已知坑（都踩过）

- 发送速率过快：本地 UDP 回环会丢首帧（出现 `CRC valid` 失败 / `Insufficient block data` 警告）。LoRa 每帧重新同步，丢样本只影响当前帧，下一帧会重新锁定。
- 近距饱和：EMlink 默认 20 dBm，1 m 内会灌爆 RTL2832U（8-bit、无 AGC）→ 实验期降到 ≤0 dBm、拉远 3–5 m 或加 20–30 dB 衰减；「白墙」瀑布就是饱和。
- ADR 会切 SF7↔SF9：一块 dongle 一套参数 → 实验期先关 ADR。
- 加密：固件默认关；一旦设了密钥，空中 payload 是密文，本工具只标注 `[密文]`，不解密。
- 带宽改 250/500 kHz 时要同步改采样率与 `os_factor`（500 kHz 需 ≥2 MS/s）。

五个坑里饱和和采样率是必踩的两个，先在纸上把参数定好再动手。
