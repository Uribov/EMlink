#!/usr/bin/env python3
"""EM-SDR 核心冒烟测试：不开 GUI，验证 rtl_sdr 采集 + FFT + 解码。

跑法（必须用 em-studio 的 venv）：
    /home/sun/EMlink/tools/em-studio/.venv/bin/python tools/em-sdr/test_core.py
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from sdr_core import SDRCore  # noqa: E402


def main() -> int:
    core = SDRCore()
    print("启动核心…", flush=True)
    core.start()

    for i in range(1, 21):
        time.sleep(1.0)
        st = core.status()
        spec = core.spectrum.snapshot()
        nodes = core.sink.snapshot()
        print(
            f"[{i:2d}s] state={st['state']:<8} "
            f"样本={st['seconds']:.1f}s "
            f"底噪={spec['floor']:6.1f} 峰值={spec['peak']:6.1f} "
            f"帧={nodes['total']} 失败={nodes['fail']} "
            f"节点={len(nodes['nodes'])}"
            + (f"  err={st['error'][:60]}" if st["error"] else ""),
            flush=True,
        )
        if i == 3 and nodes["total"] == 0:
            print("  （还没收到帧，如果你没在发数据这是正常的）", flush=True)

    core.stop()

    final = core.sink.snapshot()
    spec = core.spectrum.snapshot()
    print("\n===== 结果 =====")
    print(f"瀑布行数: {len(spec['rows'])}  （满 120 行说明持续在采）")
    print(f"频谱点数: {len(spec['spec'])}")
    print(f"总帧数  : {final['total']}")
    print(f"解帧失败: {final['fail']}")
    if final["nodes"]:
        print("\n节点明细:")
        for n in final["nodes"]:
            print(f"  node_id={n['node_id']:<4} 帧={n['frames']:<4} "
                  f"SNR={n['snr']} 长度={n['last_len']} "
                  f"类型={n['types']}")
    else:
        print("\n没有节点数据 —— 空口没有帧，或者天线/功率问题")

    if final["events"]:
        print("\n最近事件:")
        for e in final["events"][-8:]:
            print(f"  {e['t']}  {e['kind']:<10} {e['text']}")

    ok = len(spec["spec"]) > 0 and spec["peak"] > spec["floor"]
    print(f"\n{'通过' if ok else '未通过'}: 频谱{'有' if ok else '无'}信号")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
