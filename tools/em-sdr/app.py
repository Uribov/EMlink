#!/usr/bin/env python3
"""EM-SDR：EMlink 专用的轻量 SDR 频谱 + LoRa 节点监视器。

只做一件事：把 915 MHz 上 EMlink 节点的空口活动显示出来。
- 左边只有频率和增益（其余空口参数由固件固定，不给调）
- 上边频谱曲线 + 瀑布
- 下边按节点分格，显示帧数、强度、最后出现时间

启动：
    python3 tools/em-sdr/app.py
    python3 tools/em-sdr/app.py --freq 915000000 --gain 40.2

注意：运行前要保证 numpy 走系统版本，否则 GNU Radio 起不来：
    export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages
"""

from __future__ import annotations

import argparse
import os
import sys
import traceback
from pathlib import Path

# 必须在 import numpy 之前清掉 Hermes 塞进来的 _internal 目录
os.environ.pop("LD_LIBRARY_PATH", None)

import webview  # noqa: E402

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from sdr_core import SDRCore  # noqa: E402


class Api:
    """暴露给前端的 JS 桥。所有方法都返回 dict，绝不抛异常上去。"""

    def __init__(self, core: SDRCore):
        self.core = core
        self._window = None

    def attach_window(self, window):
        self._window = window

    def _safe(self, fn, *a, **kw):
        try:
            return {"ok": True, "data": fn(*a, **kw)}
        except Exception as exc:
            return {
                "ok": False,
                "data": None,
                "error": f"{type(exc).__name__}: {exc}",
                "trace": traceback.format_exc(limit=3),
            }

    # ---------- 生命周期 ----------

    def start(self):
        def _do():
            self.core.start()
            return self._status()
        return self._safe(_do)

    def stop(self):
        def _do():
            self.core.stop()
            return self._status()
        return self._safe(_do)

    def status(self):
        return self._safe(self._status)

    def _status(self) -> dict:
        st = self.core.status()
        spec = self.core.spectrum
        st["floor"] = round(spec._floor, 1)
        st["peak"] = round(spec._peak, 1)
        return st

    # ---------- 参数 ----------

    def set_freq(self, hz):
        return self._safe(lambda: self._set_freq(int(hz)))

    def _set_freq(self, hz: int) -> dict:
        if not (24_000_000 <= hz <= 1_766_000_000):
            raise ValueError(f"频率 {hz} 超出 RTL-SDR 范围")
        self.core.set_freq(hz)
        return {"freq_hz": hz}

    def set_gain(self, db):
        return self._safe(lambda: self._set_gain(float(db)))

    def _set_gain(self, db: float) -> dict:
        if not (0.0 <= db <= 49.6):
            raise ValueError(f"增益 {db} 超出范围")
        self.core.set_gain(db)
        return {"gain_db": db}

    # ---------- 数据 ----------

    def get_spectrum(self):
        return self._safe(self.core.spectrum.snapshot)

    def get_nodes(self):
        return self._safe(self.core.sink.snapshot)

    def get_node_detail(self, node_id):
        return self._safe(lambda: self.core.sink.node_detail(int(node_id)))

    # ---------- 原始 IQ 录制 ----------

    def start_record(self, seconds):
        return self._safe(lambda: self.core.start_record(float(seconds)))

    def stop_record(self):
        return self._safe(lambda: self.core.stop_record(abort=True))

    def record_status(self):
        return self._safe(self.core.record_status)

    # ---------- 关闭 ----------

    def shutdown(self):
        try:
            self.core.stop()
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="EM-SDR")
    ap.add_argument("--freq", type=int, default=915_000_000,
                    help="中心频率 Hz（默认 915000000）")
    ap.add_argument("--gain", type=float, default=40.2,
                    help="增益 dB（默认 40.2）")
    ap.add_argument("--autostart", action="store_true",
                    help="启动后立刻开始采集")
    args = ap.parse_args()

    core = SDRCore(freq_hz=args.freq, gain_db=args.gain)
    api = Api(core)

    index = str(_HERE / "ui" / "index.html")
    window = webview.create_window(
        "EM-SDR",
        index,
        js_api=api,
        width=1400,
        height=900,
        min_size=(1100, 700),
    )
    api.attach_window(window)

    try:
        window.events.closing += api.shutdown
    except Exception:
        pass

    if args.autostart:
        core.start()

    webview.start()
    core.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
