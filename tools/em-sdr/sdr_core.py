#!/usr/bin/env python3
"""EM-SDR 核心：rtl_sdr 采集 + FFT + LoRa 解码。

设计取舍
--------
- **只服务 EMlink**：SF/BW/CR/sync 由固件决定，不暴露给用户；用户只需要调频率和增益。
- **不用 SDR++**：实测它的 network_sink 输出 I≡Q 的实信号且采样重复两次，
  不是正交基带，gr-lora_sdr 永远解不出来。这里直接走 rtl_sdr 原生 u8 流。
- **1 MS/s**：BW125 下 os_factor = 8，正好是 gr-lora_sdr 需要的整数比。

数据流
------
    rtl_sdr -f F -s 1M -g G -          (子进程，u8 交错 IQ)
        │
        ├─→ _reader 线程  ─┬─→ FFT 环形缓冲 ─→ 前端频谱/瀑布
        │                  └─→ IQQueue ─→ GNU Radio 流图 ─→ 帧 ─→ 节点表

线程
----
    _reader   读子进程，切块分发（阻塞在 read）
    _decoder  GNU Radio 流图（自旋）
    _supervisor 看进程是否退出，自动重启
"""

from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

# tools/sdr 里有 emlink_frame / mavlink_print
_SDR_DIR = Path(__file__).resolve().parent.parent / "sdr"
if str(_SDR_DIR) not in sys.path:
    sys.path.insert(0, str(_SDR_DIR))

from emlink_frame import (  # noqa: E402
    PROTO_CRSF,
    PROTO_MAVLINK,
    PROTO_MSP,
    FrameDecodeError,
    Reassembler,
    decode_frame,
    format_frame,
)

try:
    from mavlink_print import decode_mavlink2, describe
    _HAVE_MAVLINK = True
except Exception:  # pragma: no cover - pymavlink 可选
    _HAVE_MAVLINK = False

# ---- 空口参数：EMlink 固件固定值，不给用户调 ----
SAMP_RATE = 1_000_000
BW = 125_000
SF = 7
CR = 1                       # gr-lora_sdr 的 1 == 空口 4/5（固件 cr=5）
SYNC_WORD = 0x12
PREAMB_LEN = 8
HAS_CRC = True
IMPL_HEAD = False
OS_FACTOR = SAMP_RATE // BW  # = 8

# ---- 频谱参数 ----
FFT_BINS = 1024              # FFT 点数
SPEC_BINS = 512              # 送给前端的点数（提高横向密度）
SPEC_FPS = 25                # 频谱刷新率
WATERFALL_ROWS = 200         # 瀑布保留行数（25 fps 下约 8 秒）

# ---- 采集参数 ----
READ_CHUNK = 65536           # 每次从 rtl_sdr 读多少字节
GAIN_STEPS = [0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7,
              16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4,
              37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6]


class NodeStats:
    """一个节点的接收状态。"""

    __slots__ = ("node_id", "frames", "last_seen", "rssi", "snr", "roles",
                 "relays", "last_len", "by_type", "log")

    MAX_LOG = 200          # 每节点保留的日志条数

    def __init__(self, node_id: int):
        self.node_id = node_id
        self.frames = 0
        self.last_seen = 0.0
        self.rssi: float | None = None
        self.snr: float | None = None
        self.roles = set()
        self.relays = set()
        self.last_len = 0
        self.by_type = {}
        self.log: list[dict] = []

    def add_log(self, entry: dict) -> None:
        self.log.append(entry)
        if len(self.log) > self.MAX_LOG:
            del self.log[:len(self.log) - self.MAX_LOG]

    def to_dict(self, now: float) -> dict:
        age = now - self.last_seen if self.last_seen else 1e9
        return {
            "node_id": self.node_id,
            "frames": self.frames,
            "age_ms": int(age * 1000),
            "rssi": round(self.rssi, 1) if self.rssi is not None else None,
            "snr": round(self.snr, 1) if self.snr is not None else None,
            "last_len": self.last_len,
            "relays": sorted(self.relays)[:8],
            "types": dict(self.by_type),
            "alive": age < 5.0,
        }


class Spectrum:
    """最新一帧频谱。

    只保留**最新一行**瀑布数据：整张瀑布由前端累积。原因是一行 512 个点
    已经够用，而把 200 行整张发过去每帧就是 10 万个数字、JSON 几百 KB，
    25 fps 下界面会直接卡死。
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._spec = [-120.0] * SPEC_BINS
        self._floor = -120.0
        self._peak = -120.0
        self._row = [0] * SPEC_BINS      # 最新一行瀑布（0..255）

    def update(self, iq: np.ndarray) -> None:
        n = min(FFT_BINS, iq.size)
        if n < 64:
            return
        seg = iq[-n:]
        win = np.hanning(n)
        spec = np.fft.fftshift(np.fft.fft(seg * win))
        # 归一化到 dBFS：满量程正弦为 0 dB，底噪约 -60 dBFS
        db = 20.0 * np.log10(np.abs(spec) / (n * 0.5) + 1e-12)

        # 降采样到 SPEC_BINS（取每个桶的最大值，保留窄带信号）
        step = n // SPEC_BINS
        if step >= 1:
            trimmed = db[:step * SPEC_BINS].reshape(SPEC_BINS, step)
            db = trimmed.max(axis=1)

        floor = float(np.percentile(db, 20))
        peak = float(db.max())

        # 瀑布映射：底噪附近为暗，往上 26 dB 逐渐到白
        lo, hi = floor - 2.0, floor + 26.0
        norm = np.clip((db - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
        row = (norm * 255).astype(np.uint8).tolist()

        with self._lock:
            self._spec = db.tolist()
            self._floor = floor
            self._peak = peak
            self._row = row

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "spec": [round(v, 1) for v in self._spec],
                "floor": round(self._floor, 1),
                "peak": round(self._peak, 1),
                "row": self._row,     # 只有最新一行，前端自己往下累积
            }


class FrameSink:
    """把解出的 LoRa 帧变成节点统计。"""

    def __init__(self, spectrum: Spectrum):
        self.nodes: dict[int, NodeStats] = {}
        self.spectrum = spectrum
        self.reasm = Reassembler()
        self.total = 0
        self.fail = 0
        self.events: list[dict] = []     # 最近事件，给前端滚动
        self._lock = threading.Lock()

    def _node(self, node_id: int) -> NodeStats:
        st = self.nodes.get(node_id)
        if st is None:
            st = NodeStats(node_id)
            self.nodes[node_id] = st
        return st

    def feed(self, packet: bytes, crc_ok) -> None:
        now = time.time()
        self.reasm.tick(now_ms=now * 1000.0)

        try:
            frame = decode_frame(packet)
        except FrameDecodeError as exc:
            with self._lock:
                self.fail += 1
                self._event_locked(now, "解帧失败", str(exc))
            return

        with self._lock:
            self.total += 1
            self._event_locked(
                now, f"帧 #{self.total}",
                f"src={frame.src_id} relay={frame.relay_id} ttl={frame.ttl} "
                f"len={len(frame.payload)}",
            )

        # 信号强度：用当前频谱相对底噪估算（gr-lora_sdr 不提供 RSSI）
        snap_floor = self.spectrum._floor
        snap_peak = self.spectrum._peak
        snr = snap_peak - snap_floor

        for node_id in (frame.src_id, frame.relay_id):
            if not node_id:
                continue
            st = self._node(node_id)
            st.frames += 1
            st.last_seen = now
            st.last_len = len(frame.payload)
            st.snr = snr
            st.rssi = snap_peak
            if frame.relay_id:
                st.relays.add(frame.relay_id)
            key = ("MAV" if frame.proto == PROTO_MAVLINK else
                   "CRSF" if frame.proto == PROTO_CRSF else
                   "MSP" if frame.proto == PROTO_MSP else "CUSTOM")
            st.by_type[key] = st.by_type.get(key, 0) + 1

            # 该节点的接收日志：谁是源、谁是中继、这一跳的详情
            role = "src" if node_id == frame.src_id else "relay"
            st.add_log({
                "t": time.strftime("%H:%M:%S", time.localtime(now)),
                "ms": int((now % 1) * 1000),
                "role": role,
                "src": frame.src_id,
                "relay": frame.relay_id,
                "ttl": frame.ttl,
                "seq": frame.seq,
                "proto": key,
                "len": len(frame.payload),
                "qos": frame.qos,
                "ack": bool(frame.ack_req),
                "frag": (f"{frame.frag_index + 1}/{frame.frag_total}"
                         if frame.frag else None),
                "enc": bool(frame.enc),
                "snr": round(snr, 1),
                "dbfs": round(snap_peak, 1),
                "hex": frame.payload.hex()[:64],
            })

    def _event_locked(self, now: float, kind: str, text: str) -> None:
        self.events.append({
            "t": time.strftime("%H:%M:%S", time.localtime(now)),
            "kind": kind,
            "text": text,
        })
        if len(self.events) > 200:
            del self.events[:len(self.events) - 200]

    def snapshot(self) -> dict:
        now = time.time()
        with self._lock:
            nodes = [st.to_dict(now) for st in self.nodes.values()]
            nodes.sort(key=lambda d: (-d["frames"], d["node_id"]))
            return {
                "nodes": nodes,
                "total": self.total,
                "fail": self.fail,
                "events": self.events[-40:],
            }

    def node_detail(self, node_id: int) -> dict:
        """单个节点的完整信息 + 接收日志（供详情页）。"""
        now = time.time()
        with self._lock:
            st = self.nodes.get(node_id)
            if st is None:
                return {"node": None, "log": []}
            return {"node": st.to_dict(now), "log": list(st.log)}


class SDRCore:
    """rtl_sdr 采集 + FFT + LoRa 解码的总控。"""

    def __init__(self, freq_hz: int = 915_000_000, gain_db: float = 40.2):
        self.freq_hz = int(freq_hz)
        self.gain_db = float(gain_db)
        self.spectrum = Spectrum()
        self.sink = FrameSink(self.spectrum)

        self._proc: subprocess.Popen | None = None
        self._running = False
        self._iq_queue: queue.Queue = queue.Queue(maxsize=64)
        self._threads: list[threading.Thread] = []
        self._status = "stopped"
        self._error = ""
        self._tb = None
        self._samples = 0
        self._lock = threading.Lock()

    # ---------- 状态 ----------

    @property
    def running(self) -> bool:
        return self._running

    def status(self) -> dict:
        with self._lock:
            return {
                "running": self._running,
                "state": self._status,
                "error": self._error,
                "freq_hz": self.freq_hz,
                "gain_db": self.gain_db,
                "samples": self._samples,
                "seconds": round(self._samples / SAMP_RATE, 1),
            }

    # ---------- 生命周期 ----------

    def start(self) -> bool:
        if self._running:
            return True
        self._running = True
        self._status = "starting"
        self._error = ""

        self._threads = [
            threading.Thread(target=self._reader_loop, daemon=True,
                             name="emsdr-reader"),
            threading.Thread(target=self._decoder_loop, daemon=True,
                             name="emsdr-decoder"),
        ]
        for t in self._threads:
            t.start()
        return True

    def stop(self) -> None:
        self._running = False
        self._status = "stopped"
        self._kill_proc()
        if self._tb is not None:
            try:
                self._tb.stop()
                self._tb.wait()
            except Exception:
                pass
            self._tb = None

    def _kill_proc(self) -> None:
        p = self._proc
        self._proc = None
        if p is None:
            return
        try:
            p.terminate()
            p.wait(timeout=2.0)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass

    # ---------- 参数 ----------

    def set_freq(self, hz: int) -> None:
        hz = int(hz)
        if hz == self.freq_hz:
            return
        self.freq_hz = hz
        if self._running:
            self._restart_proc()

    def set_gain(self, db: float) -> None:
        db = float(db)
        if abs(db - self.gain_db) < 1e-6:
            return
        self.gain_db = db
        if self._running:
            self._restart_proc()

    def _restart_proc(self) -> None:
        self._kill_proc()
        time.sleep(0.2)

    # ---------- 采集 ----------

    def _spawn(self) -> subprocess.Popen:
        env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
        cmd = [
            "rtl_sdr",
            "-f", str(self.freq_hz),
            "-s", str(SAMP_RATE),
            "-g", f"{self.gain_db:.1f}",
            "-",
        ]
        return subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, env=env,
        )

    def _reader_loop(self) -> None:
        while self._running:
            if self._proc is None:
                try:
                    self._proc = self._spawn()
                    with self._lock:
                        self._status = "running"
                        self._error = ""
                except FileNotFoundError:
                    with self._lock:
                        self._status = "error"
                        self._error = "找不到 rtl_sdr 命令"
                    time.sleep(2.0)
                    continue
                except Exception as exc:
                    with self._lock:
                        self._status = "error"
                        self._error = f"启动 rtl_sdr 失败: {exc}"
                    time.sleep(2.0)
                    continue

            proc = self._proc
            try:
                chunk = proc.stdout.read(READ_CHUNK)
            except Exception:
                chunk = b""

            if not chunk:
                # 进程没了：读 stderr 判断原因
                err = b""
                try:
                    err = proc.stderr.read() or b""
                except Exception:
                    pass
                text = err.decode("utf-8", "replace").strip()
                self._kill_proc()
                with self._lock:
                    self._status = "error"
                    self._error = text.splitlines()[-1] if text else "rtl_sdr 已退出"
                if self._running:
                    time.sleep(1.5)
                continue

            # 1. 频谱（每块都算太浪费，按块大小节流）
            iq = (np.frombuffer(chunk, dtype=np.uint8).astype(np.float32)
                  - 127.5) / 128.0
            if iq.size >= 2:
                c = iq[0::2] + 1j * iq[1::2]
                self.spectrum.update(c)
            self._samples += len(chunk) // 2

            # 2. 喂解码队列（丢了就丢了，解码慢不能反过来卡采集）
            try:
                self._iq_queue.put_nowait(chunk)
            except queue.Full:
                pass

    # ---------- 解码 ----------

    def _decoder_loop(self) -> None:
        try:
            from gnuradio import blocks, gr
            from gnuradio import lora_sdr
        except Exception as exc:
            with self._lock:
                self._error = f"GNU Radio 不可用: {exc}"
            return

        q = self._iq_queue
        sink_obj = self.sink

        class IQSource(gr.sync_block):
            """从 queue 取 u8 字节，直接输出复采样。

            输出 complex64 而不是 uint8：u8 流是 I/Q 交错，如果按字节往外送，
            GNU Radio 随时可能切在 I 和 Q 中间，后面就全反相、永远解不出。
            在这里一次转好，对齐由 out_sig 的类型保证。
            """

            def __init__(self):
                gr.sync_block.__init__(
                    self, name="emsdr_iq_source",
                    in_sig=None, out_sig=[np.complex64],
                )
                self._buf = bytearray()

            def work(self, input_items, output_items):
                out = output_items[0]
                cap = len(out)
                need = cap * 2          # 一个复采样 = 两个字节
                while len(self._buf) < need:
                    try:
                        self._buf.extend(q.get_nowait())
                    except queue.Empty:
                        break
                n = min(cap, len(self._buf) // 2)
                if n:
                    raw = np.frombuffer(
                        bytes(self._buf[:n * 2]), dtype=np.uint8,
                    ).astype(np.float32)
                    i = (raw[0::2] - 127.5) / 128.0
                    qq = (raw[1::2] - 127.5) / 128.0
                    out[:n] = i + 1j * qq
                    del self._buf[:n * 2]
                else:
                    time.sleep(0.002)   # 避免空转吃满 CPU
                return n

        class FrameSinkBlock(gr.sync_block):
            """按 frame_info tag 切包，交给 FrameSink。"""

            def __init__(self, target):
                gr.sync_block.__init__(
                    self, name="emsdr_frame_sink",
                    in_sig=[np.uint8], out_sig=None,
                )
                self.target = target
                self._buf = bytearray()
                self._buf_start = 0
                self._segs = []

            def work(self, input_items, output_items):
                import pmt
                data = input_items[0]
                n = len(data)
                abs0 = self.nitems_read(0)
                abs_end = abs0 + n
                self._buf.extend(data)

                tags = self.get_tags_in_window(
                    0, 0, n, pmt.intern("frame_info"))
                for tag in tags:
                    val = tag.value
                    pay_len = pmt.to_long(pmt.dict_ref(
                        val, pmt.intern("pay_len"), pmt.from_long(0)))
                    crc_ok = None
                    try:
                        crc_ok = pmt.to_bool(pmt.dict_ref(
                            val, pmt.intern("crc_valid"), pmt.from_bool(True)))
                    except Exception:
                        crc_ok = None
                    if pay_len > 0:
                        self._segs.append((tag.offset,
                                           tag.offset + pay_len,
                                           pay_len, crc_ok))

                while self._segs and self._segs[0][1] <= abs_end:
                    start, _end, pay_len, crc_ok = self._segs.pop(0)
                    rel = start - self._buf_start
                    if rel < 0:
                        continue
                    packet = bytes(self._buf[rel:rel + pay_len])
                    try:
                        self.target.feed(packet, crc_ok)
                    except Exception as exc:
                        import traceback
                        print(f"[FeedError] {type(exc).__name__}: {exc}",
                              flush=True)
                        traceback.print_exc()

                keep = self._segs[0][0] if self._segs else abs_end
                if keep > self._buf_start:
                    del self._buf[:keep - self._buf_start]
                    self._buf_start = keep
                return n

        try:
            tb = gr.top_block("emsdr")
            src = IQSource()

            fs = lora_sdr.frame_sync(
                int(self.freq_hz), BW, SF, IMPL_HEAD, [SYNC_WORD],
                OS_FACTOR, PREAMB_LEN,
            )
            fft = lora_sdr.fft_demod(False, True)
            gm = lora_sdr.gray_mapping(False)
            dei = lora_sdr.deinterleaver(False)
            hd = lora_sdr.hamming_dec(False)
            hdec = lora_sdr.header_decoder(
                IMPL_HEAD, CR, 255, HAS_CRC, 2, False)
            dw = lora_sdr.dewhitening()
            crc = lora_sdr.crc_verif(0, False)
            sinkblk = FrameSinkBlock(sink_obj)

            tb.connect(src, fs, fft, gm, dei, hd, hdec, dw, crc, sinkblk)
            tb.msg_connect((hdec, "frame_info"), (fs, "frame_info"))

            self._tb = tb
            tb.start()
            print("[解码] 流图已启动", flush=True)
            _reported = 0
            while self._running:
                time.sleep(1.0)
                # 每秒报一次队列深度，判断解码是否跟得上采集
                _reported += 1
                if _reported % 5 == 0:
                    print(f"[解码] 队列={self._iq_queue.qsize()} "
                          f"帧={sink_obj.total} 失败={sink_obj.fail}",
                          flush=True)
            tb.stop()
            tb.wait()
        except Exception as exc:
            import traceback
            print(f"[解码] 流图异常: {type(exc).__name__}: {exc}", flush=True)
            traceback.print_exc()
            with self._lock:
                self._error = f"解码流图异常: {exc}"
        finally:
            self._tb = None
