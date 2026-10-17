#!/usr/bin/env python3
"""EMlink Mesh 帧解析 / 分片重组（纯 Python，无硬件依赖）。

规格来源：
  firmware/src/link/frame_codec.h
  firmware/src/link/frame_codec.cpp
  firmware/src/service/frag.h
  firmware/src/service/frag.cpp
  firmware/src/service/frag.md

本模块只负责“字节 -> 结构化帧”以及分片重组，不做加密解密。
"""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass, field
from typing import Dict, Iterable, Optional, Tuple

# ---- 帧常量（照 frame_codec.h） ----
FRAME_HDR_LEN = 15
FRAME_CRC_LEN = 2
FRAME_MTU = 200
FRAME_MAX_PAYLOAD = 179
FRAME_TAG_LEN = 4
FRAME_MAX_PLAIN = 175
FRAME_DEF_TTL = 5

FRAME_DATA = 0
FRAME_ACK = 1
FRAME_NACK = 2
FRAME_BEACON = 3
FRAME_PING = 4
FRAME_ROUTE = 5
FRAME_CFG = 6

PROTO_MAVLINK = 0
PROTO_CRSF = 1
PROTO_MSP = 2
PROTO_CUSTOM = 3

FLAG_ACK_REQ = 1 << 5
FLAG_FRAG = 1 << 4
FLAG_ENC = 1 << 0
FLAG_PROTO_MASK = 0x0C

TYPE_NAMES = {
    FRAME_DATA: "DATA",
    FRAME_ACK: "ACK",
    FRAME_NACK: "NACK",
    FRAME_BEACON: "BEACON",
    FRAME_PING: "PING",
    FRAME_ROUTE: "ROUTE",
    FRAME_CFG: "CFG",
}

PROTO_NAMES = {
    PROTO_MAVLINK: "MAVLINK",
    PROTO_CRSF: "CRSF",
    PROTO_MSP: "MSP",
    PROTO_CUSTOM: "CUSTOM",
}


class FrameDecodeError(ValueError):
    """帧解码失败；str(exc) 是给人看的具体原因。"""


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE：poly=0x1021, init=0xFFFF, MSB-first, 无 final XOR。"""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass
class Frame:
    """内存中的 Mesh 帧。字段顺序与任务书一致。"""

    ver: int = 1
    type: int = FRAME_DATA
    qos: int = 0
    ack_req: bool = False
    frag: bool = False
    enc: bool = False
    proto: int = PROTO_MAVLINK
    src_id: int = 0
    dst_id: int = 0
    seq: int = 0
    ttl: int = FRAME_DEF_TTL
    relay_id: int = 0
    orig_sysid: int = 0
    net_id: int = 0
    enc_epoch: int = 0
    frag_msg_id: int = 0
    frag_index: int = 0
    frag_total: int = 0
    frag_seq: int = 0
    payload: bytes = b""
    raw: bytes = b""


def encode_frame(frame: Optional[Frame] = None, **fields) -> bytes:
    """把 Frame（或关键字字段）编码成线上字节。

    仅供测试 / 自造帧使用；正常监听链路只做解码。
    """
    if frame is None:
        frame = Frame(**fields)
    elif fields:
        raise TypeError("encode_frame 只接受一个 Frame 或关键字字段")

    payload = bytes(frame.payload or b"")
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise ValueError(
            f"payload 超长: {len(payload)} > {FRAME_MAX_PAYLOAD}"
        )

    total = FRAME_HDR_LEN + len(payload) + FRAME_CRC_LEN
    buf = bytearray(total)

    buf[0] = (1 << 4) | (int(frame.type) & 0x0F)  # ver 固定为 1
    buf[1] = (
        ((int(frame.qos) & 0x03) << 6)
        | (FLAG_ACK_REQ if frame.ack_req else 0)
        | (FLAG_FRAG if frame.frag else 0)
        | (FLAG_ENC if frame.enc else 0)
        | ((int(frame.proto) << 2) & FLAG_PROTO_MASK)
    )
    buf[2] = int(frame.src_id) & 0xFF
    buf[3] = int(frame.dst_id) & 0xFF
    buf[4] = int(frame.seq) & 0xFF
    buf[5] = (int(frame.seq) >> 8) & 0xFF
    buf[6] = int(frame.ttl) & 0xFF
    buf[7] = int(frame.relay_id) & 0xFF
    buf[8] = int(frame.orig_sysid) & 0xFF
    buf[9] = int(frame.net_id) & 0xFF
    buf[10] = int(frame.enc_epoch) & 0xFF
    buf[11] = int(frame.frag_msg_id) & 0xFF
    buf[12] = int(frame.frag_index) & 0xFF
    buf[13] = int(frame.frag_total) & 0xFF
    buf[14] = int(frame.frag_seq) & 0xFF
    buf[15:15 + len(payload)] = payload

    crc = crc16_ccitt(bytes(buf[:FRAME_HDR_LEN + len(payload)]))
    buf[total - 2] = crc & 0xFF          # 低字节在前
    buf[total - 1] = (crc >> 8) & 0xFF
    return bytes(buf)


def decode_frame(buf: bytes) -> Frame:
    """线上字节 -> Frame；失败抛 FrameDecodeError（带具体原因）。"""
    if not isinstance(buf, (bytes, bytearray, memoryview)):
        raise FrameDecodeError("输入不是字节串")
    raw = bytes(buf)

    if len(raw) < FRAME_HDR_LEN + FRAME_CRC_LEN:
        raise FrameDecodeError(
            f"长度不足: {len(raw)} < {FRAME_HDR_LEN + FRAME_CRC_LEN}"
        )

    ver = raw[0] >> 4
    if ver != 1:
        raise FrameDecodeError(f"版本错误: ver={ver} != 1")

    payload_len = len(raw) - FRAME_HDR_LEN - FRAME_CRC_LEN
    if payload_len > FRAME_MAX_PAYLOAD:
        raise FrameDecodeError(
            f"payload 超长: {payload_len} > {FRAME_MAX_PAYLOAD}"
        )

    crc_calc = crc16_ccitt(raw[:FRAME_HDR_LEN + payload_len])
    crc_recv = raw[-2] | (raw[-1] << 8)
    if crc_calc != crc_recv:
        raise FrameDecodeError(
            f"CRC 校验失败: 计算值=0x{crc_calc:04X} 帧内值=0x{crc_recv:04X}"
        )

    return Frame(
        ver=ver,
        type=raw[0] & 0x0F,
        qos=(raw[1] >> 6) & 0x03,
        ack_req=bool(raw[1] & FLAG_ACK_REQ),
        frag=bool(raw[1] & FLAG_FRAG),
        enc=bool(raw[1] & FLAG_ENC),
        proto=(raw[1] & FLAG_PROTO_MASK) >> 2,
        src_id=raw[2],
        dst_id=raw[3],
        seq=raw[4] | (raw[5] << 8),
        ttl=raw[6],
        relay_id=raw[7],
        orig_sysid=raw[8],
        net_id=raw[9],
        enc_epoch=raw[10],
        frag_msg_id=raw[11],
        frag_index=raw[12],
        frag_total=raw[13],
        frag_seq=raw[14],
        payload=raw[FRAME_HDR_LEN:FRAME_HDR_LEN + payload_len],
        raw=raw,
    )


def format_frame(f: Frame) -> str:
    """把一帧格式化成一行紧凑文本。len= 是 payload 长度。"""
    type_name = TYPE_NAMES.get(f.type, f"TYPE_{f.type}")
    proto_name = PROTO_NAMES.get(f.proto, f"PROTO_{f.proto}")

    text = (
        f"{type_name} src={f.src_id} dst={f.dst_id} relay={f.relay_id} "
        f"ttl={f.ttl} sysid={f.orig_sysid} net={f.net_id} proto={proto_name} "
        f"qos={f.qos} ack={int(f.ack_req)} frag={int(f.frag)} enc={int(f.enc)} "
        f"epoch={f.enc_epoch} seq={f.seq} len={len(f.payload)}"
    )
    if f.frag:
        text += (
            f" frag={f.frag_msg_id}/{f.frag_index}/{f.frag_total}/{f.frag_seq}"
        )
    if f.enc:
        text += " [密文]"
    return text


@dataclass
class _ReasmSlot:
    frag_total: int
    start_ms: float
    pieces: Dict[int, bytes] = field(default_factory=dict)


class Reassembler:
    """按 (src_id, orig_sysid, frag_seq) 重组分片帧。

    时间推进方式：feed(f, now_ms=...) / tick(now_ms=...)。
    不传 now_ms 时用 time.monotonic()*1000；显式传值便于自测和离线回放。
    """

    TIMEOUT_MS = 2000
    MAX_SLOTS = 4

    def __init__(self, timeout_ms: int = TIMEOUT_MS, max_slots: int = MAX_SLOTS):
        self.timeout_ms = timeout_ms
        self.max_slots = max_slots
        self._slots: Dict[Tuple[int, int, int], _ReasmSlot] = {}
        self._timeout = 0
        self._evict = 0

    @staticmethod
    def _clock(now_ms: Optional[float]) -> float:
        return time.monotonic() * 1000.0 if now_ms is None else float(now_ms)

    def _expire(self, now_ms: float) -> None:
        expired = [
            key
            for key, slot in self._slots.items()
            if now_ms - slot.start_ms > self.timeout_ms
        ]
        for key in expired:
            del self._slots[key]
            self._timeout += 1

    def tick(self, now_ms: Optional[float] = None) -> dict:
        """推进时间，清理超时组。返回当前统计。"""
        self._expire(self._clock(now_ms))
        return self.stats()

    def feed(self, f: Frame, now_ms: Optional[float] = None) -> Optional[bytes]:
        """喂一帧。

        - frag=0：payload 已是一条完整消息，直接返回。
        - frag=1：进重组，组齐时返回完整消息，否则返回 None。
        """
        now = self._clock(now_ms)
        self._expire(now)

        if not f.frag:
            return bytes(f.payload)

        total = int(f.frag_total)
        index = int(f.frag_index)
        if total <= 0 or index < 0 or index >= total:
            return None

        key = (
            int(f.src_id) & 0xFF,
            int(f.orig_sysid) & 0xFF,
            int(f.frag_seq) & 0xFF,
        )

        slot = self._slots.get(key)
        if slot is None:
            if len(self._slots) >= self.max_slots:
                victim = min(
                    self._slots,
                    key=lambda k: self._slots[k].start_ms,
                )
                del self._slots[victim]
                self._evict += 1
            slot = _ReasmSlot(frag_total=total, start_ms=now)
            self._slots[key] = slot

        slot.pieces[index] = bytes(f.payload)

        if all(i in slot.pieces for i in range(slot.frag_total)):
            full = b"".join(slot.pieces[i] for i in range(slot.frag_total))
            del self._slots[key]
            return full
        return None

    def stats(self) -> dict:
        return {
            "timeout": self._timeout,
            "evict": self._evict,
            "pending": len(self._slots),
        }


# ---------------------------------------------------------------------------
# 自测
# ---------------------------------------------------------------------------

# 一条真实的 MAVLink2 HEARTBEAT，21 字节（sys=1 comp=1 type=2 autopilot=12）。
HEARTBEAT_V2 = bytes.fromhex("fd09000000010100000000000000020c000303b331")


def _make_frag_frame(payload: bytes, index: int, total: int, *,
                     src_id: int = 3, orig_sysid: int = 1,
                     frag_seq: int = 9, msg_id: int = 1) -> Frame:
    return Frame(
        type=FRAME_DATA,
        qos=1,
        ack_req=False,
        frag=True,
        enc=False,
        proto=PROTO_MAVLINK,
        src_id=src_id,
        dst_id=0,
        seq=42 + index,
        ttl=4,
        relay_id=2,
        orig_sysid=orig_sysid,
        net_id=0,
        enc_epoch=7,
        frag_msg_id=msg_id,
        frag_index=index,
        frag_total=total,
        frag_seq=frag_seq,
        payload=payload,
    )


def _selftest() -> int:
    checks = 0

    # 1. CRC 标准向量
    assert crc16_ccitt(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE check 失败"
    checks += 1
    print("[1] CRC 标准向量 OK: crc16_ccitt(b'123456789') == 0x29B1")

    # 2. DATA + HEARTBEAT 往返，逐字段断言
    assert len(HEARTBEAT_V2) == 21
    src = Frame(
        type=FRAME_DATA,
        qos=1,
        ack_req=True,
        frag=False,
        enc=False,
        proto=PROTO_MAVLINK,
        src_id=3,
        dst_id=0,
        seq=42,
        ttl=4,
        relay_id=2,
        orig_sysid=1,
        net_id=0,
        enc_epoch=7,
        frag_msg_id=0,
        frag_index=0,
        frag_total=0,
        frag_seq=0,
        payload=HEARTBEAT_V2,
    )
    wire = encode_frame(src)
    assert len(wire) == 15 + 21 + 2
    decoded = decode_frame(wire)
    for name in (
        "ver", "type", "qos", "ack_req", "frag", "enc", "proto",
        "src_id", "dst_id", "seq", "ttl", "relay_id", "orig_sysid",
        "net_id", "enc_epoch", "frag_msg_id", "frag_index",
        "frag_total", "frag_seq", "payload",
    ):
        assert getattr(decoded, name) == getattr(src, name), name
    assert decoded.raw == wire
    checks += 1
    print("[2] DATA + HEARTBEAT 编码/解码往返 OK，20 个字段逐项相等")

    # 3. 篡改任意 1 字节（每个位置 XOR 1）都必须因 CRC 失败
    for pos in range(len(wire)):
        bad = bytearray(wire)
        bad[pos] ^= 0x01
        try:
            decode_frame(bytes(bad))
        except FrameDecodeError as exc:
            assert "CRC" in str(exc), (pos, str(exc))
        else:
            raise AssertionError(f"篡改偏移 {pos} 后竟然解码成功")
    checks += 1
    print(f"[3] 逐字节篡改 {len(wire)} 个位置，全部以 CRC 原因失败 OK")

    # 4. 长度不足 / ver!=1 给出可区分原因
    try:
        decode_frame(wire[:16])
    except FrameDecodeError as exc:
        assert "长度" in str(exc), str(exc)
    else:
        raise AssertionError("短帧未失败")

    bad_ver = bytearray(wire)
    bad_ver[0] = (2 << 4) | (bad_ver[0] & 0x0F)
    try:
        decode_frame(bytes(bad_ver))
    except FrameDecodeError as exc:
        assert "版本" in str(exc), str(exc)
        assert "长度" not in str(exc) and "CRC" not in str(exc), str(exc)
    else:
        raise AssertionError("ver!=1 未失败")
    checks += 1
    print("[4] 长度不足 / 版本错误 的失败原因可区分 OK")

    # 5. 200B 消息拆 2 片（175+25），乱序重组仍逐字节相等
    message = bytes((i * 7 + 3) & 0xFF for i in range(200))
    p0, p1 = message[:175], message[175:]
    assert len(p0) == 175 and len(p1) == 25
    f0 = _make_frag_frame(p0, 0, 2)
    f1 = _make_frag_frame(p1, 1, 2)
    # 走一遍真实的线上编解码
    f0 = decode_frame(encode_frame(f0))
    f1 = decode_frame(encode_frame(f1))

    reasm = Reassembler()
    assert reasm.feed(f0, now_ms=1000) is None
    assert reasm.feed(f1, now_ms=1001) == message
    assert reasm.stats() == {"timeout": 0, "evict": 0, "pending": 0}

    reasm = Reassembler()
    assert reasm.feed(f1, now_ms=1000) is None  # 乱序：先喂末片
    assert reasm.feed(f0, now_ms=1001) == message
    checks += 1
    print("[5] 200B -> 175+25 正序/乱序重组均逐字节相等 OK")

    # 6. 缺片超时：只喂第 0 片，tick(+2001ms) 后 timeout=1 且槽清空
    reasm = Reassembler()
    assert reasm.feed(f0, now_ms=1000) is None
    assert reasm.stats()["pending"] == 1
    st = reasm.tick(now_ms=3001)
    assert st["timeout"] == 1, st
    assert st["pending"] == 0, st
    checks += 1
    print("[6] 缺片 2001ms 超时 -> timeout=1, pending=0 OK")

    # 7. 不同 src_id 但同 orig_sysid/frag_seq 的组不得互相污染
    msg_a = bytes((i * 11 + 1) & 0xFF for i in range(200))
    msg_b = bytes((i * 13 + 200) & 0xFF for i in range(200))
    a0 = _make_frag_frame(msg_a[:175], 0, 2, src_id=3, orig_sysid=1, frag_seq=9)
    a1 = _make_frag_frame(msg_a[175:], 1, 2, src_id=3, orig_sysid=1, frag_seq=9)
    b0 = _make_frag_frame(msg_b[:175], 0, 2, src_id=4, orig_sysid=1, frag_seq=9)
    b1 = _make_frag_frame(msg_b[175:], 1, 2, src_id=4, orig_sysid=1, frag_seq=9)

    reasm = Reassembler()
    assert reasm.feed(a0, now_ms=1000) is None
    assert reasm.feed(b0, now_ms=1001) is None
    assert reasm.feed(a1, now_ms=1002) == msg_a
    assert reasm.feed(b1, now_ms=1003) == msg_b
    assert reasm.stats()["pending"] == 0
    checks += 1
    print("[7] src_id 不同的同 sysid/frag_seq 组互不污染 OK")

    print(f"--selftest PASS ({checks} 项)")
    return 0


def main(argv: Iterable[str]) -> int:
    args = list(argv)
    if "--selftest" in args:
        return _selftest()
    print("用法: python3 tools/sdr/emlink_frame.py --selftest", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
