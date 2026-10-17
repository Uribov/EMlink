#!/usr/bin/env python3
"""MAVLink 2 解码与一行格式化（供 SDR 监听链路使用）。

EMlink 只发送 MAVLink 2，所以固定使用 pymavlink 的 v20 方言：
  from pymavlink.dialects.v20 import ardupilotmega as mavlink2

不要用 mavutil.mavlink：它默认是 v1.0 方言（首字节 0xFE）。
"""

from __future__ import annotations

import sys
from typing import Iterable, List, Optional, Tuple

try:
    from pymavlink.dialects.v20 import ardupilotmega as mavlink2
except ImportError as _exc:  # 允许只 import 模块而不立即使用 pymavlink
    mavlink2 = None  # type: ignore[assignment]
    _IMPORT_ERROR: Optional[BaseException] = _exc
else:
    _IMPORT_ERROR = None

MAVLINK2_STX = 0xFD
MAVLINK2_HDR_LEN = 10
MAVLINK2_CRC_LEN = 2
MAVLINK2_SIG_LEN = 13

# pymavlink 的 fieldunits_by_name 里常见的缩放单位。
_UNIT_SCALE = {
    "degE7": 1e-7,
    "degE5": 1e-5,
    "mm": 1e-3,
    "cm": 1e-2,
    "cm/s": 1e-2,
    "dm/s": 1e-1,
    "m/s": 1.0,
    "cdeg": 1e-2,
    "cdeg/s": 1e-2,
    "ms": 1.0,
    "us": 1e-3,
}

_MAX_FIELDS = 6


def _require_pymavlink() -> None:
    if mavlink2 is None:
        raise RuntimeError(
            "需要 pymavlink（当前 Python 未安装）。"
            "请用装有 pymavlink 的解释器，例如 /usr/bin/python3。"
        ) from _IMPORT_ERROR


def _new_mav() -> "mavlink2.MAVLink":
    _require_pymavlink()
    return mavlink2.MAVLink(None)


def _frame_total_len(buf: bytes, pos: int) -> Optional[int]:
    """若 pos 处像 MAVLink2 帧头，返回整帧长度；头不完整返回 None。"""
    if pos + MAVLINK2_HDR_LEN > len(buf):
        return None
    payload_len = buf[pos + 1]
    incompat_flags = buf[pos + 2]
    total = MAVLINK2_HDR_LEN + payload_len + MAVLINK2_CRC_LEN
    if incompat_flags & 0x01:  # signed
        total += MAVLINK2_SIG_LEN
    return total


def decode_mavlink2(buf: bytes) -> Tuple[List[object], int]:
    """解析一段字节流，返回 (消息列表, 无法消费的字节数)。

    行为：
    - 只认 MAVLink2（STX=0xFD）；v1 帧按未消费字节处理。
    - 自动跳过前导垃圾；遇到截断帧 / CRC 错 / 未知消息都不抛异常。
    - 无法消费字节数 = 总字节 - 所有成功解析帧的字节数。
    """
    _require_pymavlink()
    data = bytes(buf)
    n = len(data)
    if n == 0:
        return [], 0

    messages: List[object] = []
    consumed = 0
    i = 0

    while i < n:
        if data[i] != MAVLINK2_STX:
            i += 1
            continue

        total = _frame_total_len(data, i)
        if total is None or i + total > n:
            # 截断帧头/帧体：不能确定是有效帧，跳 1 字节继续找下一个 STX。
            i += 1
            continue

        frame = data[i:i + total]
        try:
            mav = _new_mav()
            parsed = mav.parse_buffer(frame)
        except Exception:
            # pymavlink 对坏前缀/坏 CRC/未知消息都可能抛异常；一律当无效帧。
            parsed = None

        if parsed:
            messages.extend(parsed)
            consumed += total
            i += total
        else:
            i += 1

    return messages, n - consumed


def _fmt_value(value: object) -> str:
    if isinstance(value, float):
        text = f"{value:.7f}".rstrip("0").rstrip(".")
        return text if text not in ("", "-0") else "0"
    return str(value)


def _format_field(msg: object, name: str) -> Optional[str]:
    try:
        value = getattr(msg, name)
    except AttributeError:
        return None

    # 过滤数组 / 二进制 / 大字段
    if isinstance(value, (bytes, bytearray, list, tuple, dict)):
        return None
    if isinstance(value, str):
        if len(value) > 40:
            return None
        return f"{name}={value}"

    units = getattr(msg, "fieldunits_by_name", None) or {}
    scale = _UNIT_SCALE.get(units.get(name, ""))
    if scale is not None and isinstance(value, (int, float)):
        value = value * scale

    return f"{name}={_fmt_value(value)}"


def describe(msg: object) -> str:
    """把一条 MAVLink 消息格式化成一行（最多 6 个短字段）。"""
    try:
        msg_type = msg.get_type()
    except Exception:
        msg_type = type(msg).__name__

    try:
        sys_id = msg.get_srcSystem()
        comp_id = msg.get_srcComponent()
    except Exception:
        sys_id = comp_id = "?"

    parts = [str(msg_type), f"sys={sys_id}", f"comp={comp_id}"]

    try:
        names = list(msg.get_fieldnames())
    except Exception:
        names = []

    shown = 0
    for name in names:
        if shown >= _MAX_FIELDS:
            break
        field_text = _format_field(msg, name)
        if field_text is None:
            continue
        parts.append(field_text)
        shown += 1

    return " ".join(parts)


# ---------------------------------------------------------------------------
# 自测
# ---------------------------------------------------------------------------


def _selftest() -> int:
    _require_pymavlink()
    checks = 0

    tx = mavlink2.MAVLink(None, srcSystem=1, srcComponent=1)
    heartbeat = mavlink2.MAVLink_heartbeat_message(
        type=2, autopilot=12, base_mode=0, custom_mode=0,
        system_status=3, mavlink_version=3,
    )
    hb_bytes = heartbeat.pack(tx)
    assert hb_bytes[0] == 0xFD and len(hb_bytes) == 21

    global_pos = mavlink2.MAVLink_global_position_int_message(
        time_boot_ms=1000,
        lat=399000000,
        lon=1164000000,
        alt=120000,
        relative_alt=100000,
        vx=0,
        vy=0,
        vz=0,
        hdg=0,
    )
    gp_bytes = global_pos.pack(tx)
    assert gp_bytes[0] == 0xFD

    msgs, unused = decode_mavlink2(hb_bytes)
    assert len(msgs) == 1, len(msgs)
    assert unused == 0, unused
    hb_text = describe(msgs[0])
    assert "HEARTBEAT" in hb_text, hb_text
    assert "sys=1" in hb_text, hb_text
    checks += 1
    print(f"[1] HEARTBEAT OK: {hb_text}")

    msgs, unused = decode_mavlink2(gp_bytes)
    assert len(msgs) == 1, len(msgs)
    assert unused == 0, unused
    gp_text = describe(msgs[0])
    assert "GLOBAL_POSITION_INT" in gp_text, gp_text
    assert "sys=1" in gp_text, gp_text
    checks += 1
    print(f"[2] GLOBAL_POSITION_INT OK: {gp_text}")

    # 两条消息拼接
    msgs, unused = decode_mavlink2(hb_bytes + gp_bytes)
    assert len(msgs) == 2, len(msgs)
    assert unused == 0, unused
    checks += 1
    print("[3] 连续两条消息拼接解析 OK")

    # 20 字节垃圾：不抛异常；空列表或全部算未消费
    garbage = bytes(range(20))
    msgs, unused = decode_mavlink2(garbage)
    assert msgs == [], msgs
    assert unused == len(garbage), (unused, len(garbage))
    checks += 1
    print(f"[4] 20B 垃圾不崩溃: msgs=0, unused={unused}")

    # 半截帧：不崩溃
    truncated = hb_bytes[:-5]
    msgs, unused = decode_mavlink2(truncated)
    assert msgs == [], msgs
    assert unused == len(truncated), (unused, len(truncated))
    checks += 1
    print(f"[5] 截断帧不崩溃: msgs=0, unused={unused}")

    # 垃圾 + 有效帧：应能跳过前导垃圾
    msgs, unused = decode_mavlink2(b"junk" + hb_bytes)
    assert len(msgs) == 1, len(msgs)
    assert unused == 4, unused
    checks += 1
    print("[6] 前导垃圾 + HEARTBEAT 可正确跳过 OK")

    print(f"--selftest PASS ({checks} 项)")
    return 0


def _main(argv: Iterable[str]) -> int:
    args = list(argv)
    if "--selftest" in args:
        return _selftest()

    # 可选：从 stdin 读 hex（每行一段），解码后逐条打印。
    if args:
        sources = args
    else:
        sources = list(sys.stdin)

    for line in sources:
        cleaned = "".join(
            ch for ch in line.strip() if ch in "0123456789abcdefABCDEF"
        )
        if not cleaned:
            continue
        if len(cleaned) % 2:
            print(f"跳过奇数长度 hex: {line.strip()}", file=sys.stderr)
            continue
        try:
            data = bytes.fromhex(cleaned)
        except ValueError:
            print(f"跳过非法 hex: {line.strip()}", file=sys.stderr)
            continue

        msgs, unused = decode_mavlink2(data)
        for msg in msgs:
            print(describe(msg))
        if not msgs:
            print(f"(无可解析 MAVLink2 消息, 未消费 {unused} 字节)")

    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
