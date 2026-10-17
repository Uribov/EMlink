"""MAVLink v2 强行编解码：组帧、拆帧、CRC-16/MCRF4XX。

拆帧只按帧结构硬拆，不依赖方言消息定义；CRC 的 crc_extra 优先从
pymavlink 方言表读取，查不到时按 0 计算（与固件 mav_crc_ok 一致）。
"""

import threading
import time

STX = 0xFD
HEADER_LEN = 10
CRC_LEN = 2

_seq_lock = threading.Lock()
_seq = 0

_tables_lock = threading.Lock()
_tables = None


def _accumulate(byte, crc):
    tmp = (byte ^ (crc & 0xFF)) & 0xFF
    tmp = (tmp ^ ((tmp << 4) & 0xFF)) & 0xFF
    return ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF


def crc16_mcrf4xx(data, crc=0xFFFF):
    for byte in data:
        crc = _accumulate(byte, crc)
    return crc & 0xFFFF


def _target_offsets(cls):
    names = getattr(cls, "fieldnames", None)
    orders = getattr(cls, "orders", None)
    lengths = getattr(cls, "lengths", None)
    if not names or not orders or not lengths:
        return None
    arrays = getattr(cls, "array_lengths", None) or []

    def field_size(index):
        size = lengths[index] if index < len(lengths) else 0
        count = arrays[index] if index < len(arrays) else 0
        return size * (count or 1)

    offsets = {}
    for field in ("target_system", "target_component"):
        if field not in names:
            continue
        index = names.index(field)
        order = orders[index]
        offsets[field] = sum(
            field_size(i) for i in range(len(names)) if orders[i] < order
        )
    return offsets or None


def _load_tables():
    global _tables
    if _tables is not None:
        return _tables
    with _tables_lock:
        if _tables is not None:
            return _tables

        extras = {}
        offsets = {}
        try:
            from pymavlink.dialects.v20 import common as dialect
        except Exception:
            _tables = (extras, offsets)
            return _tables

        msg_map = getattr(dialect, "mavlink_map", None)
        if isinstance(msg_map, dict):
            for msgid, cls in msg_map.items():
                extra = getattr(cls, "crc_extra", None)
                if extra is not None:
                    extras[int(msgid)] = int(extra)
                target = _target_offsets(cls)
                if target:
                    offsets[int(msgid)] = target

        if not extras:
            table = getattr(dialect, "MAVLINK_MESSAGE_CRCS", None)
            if isinstance(table, dict):
                items = table.items()
            elif isinstance(table, (list, tuple)):
                items = enumerate(table)
            else:
                items = ()
            for msgid, value in items:
                if isinstance(value, dict):
                    value = value.get("crc_extra", value.get("crc"))
                if isinstance(value, int):
                    extras[int(msgid)] = int(value)

        _tables = (extras, offsets)
        return _tables


def crc_extra_for(msgid):
    extras, _ = _load_tables()
    try:
        return extras.get(int(msgid), 0)
    except (TypeError, ValueError):
        return 0


def _next_seq():
    global _seq
    with _seq_lock:
        value = _seq
        _seq = (_seq + 1) & 0xFF
        return value


def _set_seq(value):
    global _seq
    with _seq_lock:
        _seq = (value + 1) & 0xFF


def _apply_target(payload, msgid, target_sysid):
    if target_sysid is None:
        return payload
    _, offsets = _load_tables()
    target = offsets.get(int(msgid))
    if not target:
        return payload
    system_ofs = target.get("target_system")
    if system_ofs is None or system_ofs >= len(payload):
        return payload
    data = bytearray(payload)
    data[system_ofs] = int(target_sysid) & 0xFF
    component_ofs = target.get("target_component")
    if component_ofs is not None and component_ofs < len(data):
        data[component_ofs] = 0
    return bytes(data)


def build_frame(payload, msgid, sysid=255, compid=0, seq=None, target_sysid=None):
    if not isinstance(payload, (bytes, bytearray)):
        raise ValueError("payload 必须是 bytes")
    msgid = int(msgid)
    if msgid < 0 or msgid > 0xFFFFFF:
        raise ValueError("msgid 必须在 0..16777215")
    sysid = int(sysid)
    compid = int(compid)
    if sysid < 0 or sysid > 0xFF:
        raise ValueError("sysid 必须在 0..255")
    if compid < 0 or compid > 0xFF:
        raise ValueError("compid 必须在 0..255")

    payload = _apply_target(bytes(payload), msgid, target_sysid)
    if len(payload) > 0xFF:
        raise ValueError("payload 最多 255 字节")

    if seq is None:
        seq = _next_seq()
    else:
        seq = int(seq) & 0xFF
        _set_seq(seq)

    header = bytes(
        (
            STX,
            len(payload),
            0,
            0,
            seq,
            sysid & 0xFF,
            compid & 0xFF,
            msgid & 0xFF,
            (msgid >> 8) & 0xFF,
            (msgid >> 16) & 0xFF,
        )
    )
    crc = crc16_mcrf4xx(header[1:] + payload)
    crc = _accumulate(crc_extra_for(msgid), crc)
    return header + payload + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def _decode_frame(frame):
    payload_len = frame[1]
    msgid = frame[7] | (frame[8] << 8) | (frame[9] << 16)
    payload = frame[HEADER_LEN:HEADER_LEN + payload_len]
    crc_recv = frame[HEADER_LEN + payload_len] | (
        frame[HEADER_LEN + payload_len + 1] << 8
    )
    crc_calc = crc16_mcrf4xx(frame[1:HEADER_LEN + payload_len])
    crc_calc = _accumulate(crc_extra_for(msgid), crc_calc)
    text = payload.decode("utf-8", "replace")
    return {
        "len": payload_len,
        "seq": frame[4],
        "sysid": frame[5],
        "compid": frame[6],
        "msgid": msgid,
        "incompat": frame[2],
        "compat": frame[3],
        "payload": payload,
        "payload_hex": payload.hex(),
        "payload_text": text,
        "crc_ok": crc_calc == crc_recv,
        "crc_recv": crc_recv,
        "crc_calc": crc_calc,
        "crc_extra": crc_extra_for(msgid),
        "ts": int(time.time() * 1000),
    }


def parse_frames(data):
    frames = []
    buf = bytes(data or b"")
    total = len(buf)
    pos = 0

    while pos < total:
        start = buf.find(bytes((STX,)), pos)
        if start < 0:
            return frames, b""
        if total - start < 2:
            return frames, buf[start:]
        payload_len = buf[start + 1]
        frame_len = HEADER_LEN + payload_len + CRC_LEN
        if total - start < frame_len:
            return frames, buf[start:]
        frames.append(_decode_frame(buf[start:start + frame_len]))
        pos = start + frame_len

    return frames, b""


def frame_to_json(frame):
    out = dict(frame)
    out.pop("payload", None)
    return out
