# EM-SDR

EMlink 专用的轻量 SDR 频谱 + LoRa 节点监视器。替代 SDR++。

## 为什么不用 SDR++

实测它的 `network_sink` **输出假 IQ**：I 和 Q 逐个样本完全相等（实信号），
而且每个采样重复两次。gr-lora_sdr 需要正交基带，这种数据永远解不出来。

同时 sink 采样率上限只有 192000，而 `192000 / 125000 = 1.536` 不是整数，
得额外插一级有理重采样，链路又长又脆。

## 它做什么

只做一件事：把 915 MHz 上 EMlink 节点的空口活动显示出来。

- **左栏**：只有频率和增益。SF/BW/CR/sync 由固件固定，不暴露 —— 调错了只会解不出来
- **上方**：频谱柱状图（256 根柱子铺满，512 及以上画布会自适应变密；超过底噪 6 dB 的柱用亮白，其余中灰）+ 瀑布（灰阶，新数据在上）
- **下方**：按 `node_id` 分格的节点卡片，显示帧数、SNR、dBFS、帧长、最后出现时间、帧类型

## 跑起来

```bash
bash tools/em-sdr/run.sh
```

或者启动即采集：

```bash
bash tools/em-sdr/run.sh --autostart
```

指定频率和增益：

```bash
bash tools/em-sdr/run.sh --freq 915000000 --gain 40.2
```

## 依赖说明

**共用 `tools/em-studio/.venv`**。原因：

- 系统 python3 没装 pywebview，pip 装系统会被 PEP 668 拦
- 那个 venv 里已经有 pywebview + **numpy 1.26.4** + 可用的 gnuradio

**numpy 必须 1.x**。GNU Radio 是用 numpy 1.x 编的，2.x 会直接 `import` 失败。
`~/.local` 里如果装了 numpy 2.x，`run.sh` 里的 `PYTHONPATH` 会把它压住。

**必须清 `LD_LIBRARY_PATH`**。Hermes 运行时把自己的 `_internal` 塞进去，里面有旧版
libstdc++，会让 GTK 后端报 `GLIBCXX_3.4.31 not found`。`run.sh` 已处理。

**rtl_sdr 独占 USB dongle**。SDR++、GQRX 之类开着的时候它会报
`usb_claim_interface error -6`。`run.sh` 会自动关掉 SDR++。

## 空口参数（写死在 `sdr_core.py`）

| 参数 | 值 | 说明 |
|---|---|---|
| 采样率 | 1 MS/s | `os_factor = 1000000 / 125000 = 8`，gr-lora_sdr 要求整数 |
| 带宽 | 125 kHz | 固件 BW125 |
| 扩频因子 | SF7 | 固件 SF7 |
| 编码率 | CR 4/5 | gr-lora_sdr 的 `cr=1` |
| 同步字 | 0x12 | 固件私有同步字 |
| 前导 | 8 | |
| 显式头 | 否 | |
| CRC | 开 | |

改这些要和固件同步改，否则解不出来。

## 数据流

```
rtl_sdr -f F -s 1M -g G -          (子进程，u8 交错 IQ)
    │
    ├─→ reader 线程 ─┬─→ FFT ─→ 前端频谱/瀑布
    │                └─→ 队列 ─→ GNU Radio 流图 ─→ 帧 ─→ 节点表
    │
    └─→ 进程退出时自动重启
```

采集和解码是**两个线程**。解码慢的时候 IQ 队列会丢数据，这是故意的 ——
宁可丢帧也不能反过来卡住采集，否则频谱会卡成幻灯片。

## 强度显示的诚实说明

界面上的 `dBFS` 是**相对满量程的 dB**，不是真实 dBm。

`gr-lora_sdr` 不提供 RSSI，所以节点卡片上的 `dBFS`/`SNR` 是拿**当前频谱的峰值和底噪**估算的：
- `dBFS` = 频谱峰值
- `SNR` = 峰值 − 底噪

它是"这帧来的时候信号有多强"的近似值，够用来判断远近和链路好坏，但不能当仪表读数用。
要真实 dBm 得做校准（已知功率源 + 固定增益），目前没做。

## 文件

| 文件 | 作用 |
|---|---|
| `app.py` | pywebview 窗口 + JS 桥 |
| `sdr_core.py` | rtl_sdr 采集、FFT、GNU Radio 解码、节点统计 |
| `test_core.py` | 不开 GUI 的核心冒烟测试 |
| `run.sh` | 启动器（环境变量、独占检查） |
| `ui/index.html` | 布局 |
| `ui/emsdr.css` | 样式（延续 EM Studio） |
| `ui/app.js` | 频谱/瀑布渲染、节点卡片、轮询 |

## 冒烟测试

不开 GUI 直接验核心：

```bash
unset LD_LIBRARY_PATH
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
tools/em-studio/.venv/bin/python tools/em-sdr/test_core.py
```

20 秒后打印底噪、峰值、帧数、节点明细。持续采集时瀑布应该满 120 行。

## 已知问题

- **SNR/dBFS 是估算值**，不是真实测量（见上）
- **没有发射能力**，纯接收
- **瀑布只有 120 行**（约 8 秒 @ 15 fps），滚动很快，看长周期信标要降刷新率
- **频率滑块拖动时不生效**，松手才重启 rtl_sdr（避免拖动过程中反复重启）
