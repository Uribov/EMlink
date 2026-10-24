#!/usr/bin/env python3
"""EMlink Mesh 实时监听：LoRa 接收流图 -> Mesh 帧 -> MAVLink 打印。

输入既可以是 make_test_iq.py 造的文件，也可以是 SDR++ network_sink 推到
UDP 的 int16 交错 IQ 流。

用法（必须用带 gnuradio / pymavlink 的解释器，见 S2 任务书 §3）：
    /usr/bin/python3 tools/sdr/lora_mesh_live.py --file tools/sdr/_selftest.i16 --limit 4 --hex
    /usr/bin/python3 tools/sdr/lora_mesh_live.py --udp 4242
"""

from __future__ import annotations

import argparse
import sys
import time

import numpy as np
import pmt
from gnuradio import blocks, filter, gr, network
from gnuradio import lora_sdr

from emlink_frame import (
    FrameDecodeError,
    Reassembler,
    decode_frame,
    format_frame,
)
from mavlink_print import decode_mavlink2, describe

# ---- 空口参数（必须与 make_test_iq.py 一致） ----
SAMP_RATE = 1_000_000
BW = 125_000
SF = 7
CR = 1                     # gr-lora_sdr 的 1 = 空口 4/5 = 固件 cr=5
SYNC_WORD = 0x12
IMPL_HEAD = False
HAS_CRC = True
PREAMB_LEN = 8
CENTER_FREQ = 915_000_000
OS_FACTOR = int(SAMP_RATE / BW)          # = 8
LDRO = 0


class Stats:
    def __init__(self):
        self.frames = 0
        self.decode_fail = 0
        self.mav_msgs = 0
        self.reassembled = 0
        self.crc_bad = 0


class MeshPipeline:
    """把一条 LoRa packet 变成帧文本 / MAVLink 文本。"""

    def __init__(self, show_hex: bool = False, quiet: bool = False):
        self.show_hex = show_hex
        self.quiet = quiet
        self.stats = Stats()
        self.reasm = Reassembler()

    @staticmethod
    def _now_ms() -> float:
        return time.monotonic() * 1000.0

    def _print(self, text: str) -> None:
        print(text, flush=True)

    def _parse_mavlink(self, payload: bytes) -> None:
        if not payload:
            return
        msgs, unused = decode_mavlink2(payload)
        for msg in msgs:
            self.stats.mav_msgs += 1
            self._print("  └ " + describe(msg))
        if unused:
            self._print(f"  └ [非 MAVLink] {unused} 字节未消费")

    def handle(self, packet: bytes, crc_valid=None) -> None:
        self.stats.frames += 1
        self.reasm.tick(now_ms=self._now_ms())

        if crc_valid is False:
            self.stats.crc_bad += 1
            self._print(f"[LoRa CRC 失败] {len(packet)} 字节（仍尝试解帧）")

        try:
            frame = decode_frame(packet)
        except FrameDecodeError as exc:
            self.stats.decode_fail += 1
            self._print(f"[解帧失败] {exc}")
            if self.show_hex:
                self._print(f"  raw: {packet.hex()}")
            return

        self._print(format_frame(frame))
        if self.show_hex:
            self._print(f"  payload: {frame.payload.hex()}")

        if frame.enc:
            self._print("  └ [密文] 未解析 payload")
            return

        if frame.frag:
            full = self.reasm.feed(frame, now_ms=self._now_ms())
            if full is None:
                self._print(
                    f"  └ [分片] {frame.frag_index + 1}/{frame.frag_total} "
                    f"已收 {len(frame.payload)} 字节"
                )
                return
            self.stats.reassembled += 1
            self._print(f"[重组] 完整消息 {len(full)} 字节")
            if self.show_hex:
                self._print(f"  payload: {full.hex()}")
            payload = full
        else:
            payload = frame.payload

        self._parse_mavlink(payload)


class MeshPacketSink(gr.sync_block):
    """按 frame_info / crc_valid tag 切出“一帧一包”的 byte sink。"""

    def __init__(self, pipeline: MeshPipeline, limit=None, tb=None,
                 verbose: bool = True):
        gr.sync_block.__init__(
            self, name="emlink_mesh_sink", in_sig=[np.uint8], out_sig=None,
        )
        self.pipeline = pipeline
        self.limit = limit
        self.tb = tb
        self.verbose = verbose
        self._buf = bytearray()
        self._buf_start = 0
        self._segments = []
        self._stopped = False

    def _handle_packet(self, packet: bytes, crc_valid) -> None:
        self.pipeline.handle(packet, crc_valid)
        if self.limit is not None and self.pipeline.stats.frames >= self.limit:
            if not self._stopped:
                self._stopped = True
                if self.verbose:
                    print(
                        f"[limit] 已收到 {self.pipeline.stats.frames} 条 mesh 帧，停止",
                        flush=True,
                    )
                if self.tb is not None:
                    self.tb.stop()

    def work(self, input_items, output_items):
        data = input_items[0]
        n = len(data)
        abs0 = self.nitems_read(0)
        abs_end = abs0 + n

        self._buf.extend(data)

        tags = self.get_tags_in_window(
            0, 0, n, pmt.intern("frame_info"),
        )
        for tag in tags:
            value = tag.value
            pay_len = pmt.to_long(pmt.dict_ref(
                value, pmt.intern("pay_len"), pmt.from_long(0),
            ))
            crc_valid = None
            try:
                crc_valid = pmt.to_bool(pmt.dict_ref(
                    value, pmt.intern("crc_valid"), pmt.from_bool(True),
                ))
            except Exception:
                crc_valid = None

            if self.verbose and pay_len <= 0:
                print(
                    f"[tag] frame_info offset={tag.offset} pay_len={pay_len} "
                    f"keys={pmt.write_string(value)}",
                    flush=True,
                )
            if pay_len > 0:
                self._segments.append(
                    (tag.offset, tag.offset + pay_len, pay_len, crc_valid),
                )

        while self._segments and self._segments[0][1] <= abs_end:
            start, end, pay_len, crc_valid = self._segments.pop(0)
            rel = start - self._buf_start
            if rel < 0:
                continue
            packet = bytes(self._buf[rel:rel + pay_len])
            self._handle_packet(packet, crc_valid)

        keep_from = self._segments[0][0] if self._segments else abs_end
        if keep_from > self._buf_start:
            del self._buf[:keep_from - self._buf_start]
            self._buf_start = keep_from

        return n


def build_flowgraph(args, pipeline: MeshPipeline):
    tb = gr.top_block("emlink_lora_mesh_live")

    if args.file:
        if args.format == "i16":
            src = blocks.file_source(gr.sizeof_short, args.file, False)
            i2c = blocks.interleaved_short_to_complex(
                False, False, 1.0 / 32768.0,
            )
            head = (src, i2c)
        elif args.format == "u8":
            # rtl_sdr 原生输出：8-bit 无符号交错，中心 127.5
            src = blocks.file_source(gr.sizeof_char, args.file, False)
            c2c = blocks.interleaved_char_to_complex(False, 1.0 / 128.0)
            offset = blocks.add_const_cc(-127.5 / 128.0)
            head = (src, c2c, offset)
        else:
            src = blocks.file_source(gr.sizeof_gr_complex, args.file, False)
            head = (src,)
    else:
        if args.format == "i16":
            itemsize = gr.sizeof_short
        elif args.format == "u8":
            itemsize = gr.sizeof_char
        else:
            itemsize = gr.sizeof_gr_complex
        src = network.udp_source(
            itemsize, 1, args.udp, 0, args.udp_payload, False, False, False,
        )
        if args.format == "i16":
            i2c = blocks.interleaved_short_to_complex(
                False, False, 1.0 / 32768.0,
            )
            head = (src, i2c)
        elif args.format == "u8":
            c2c = blocks.interleaved_char_to_complex(False, 1.0 / 128.0)
            offset = blocks.add_const_cc(-127.5 / 128.0)
            head = (src, c2c, offset)
        else:
            head = (src,)

    # 源侧采样率与 SAMP_RATE 不一致时先做有理重采样
    # （SDR++ 的 network_sink 采样率上限只有 192000，直接喂进 frame_sync 会因 os_factor 非整数而失败）
    if getattr(args, "resample_from", None) and args.resample_from != SAMP_RATE:
        import math

        g = math.gcd(int(args.resample_from), SAMP_RATE)
        interp = SAMP_RATE // g
        decim = int(args.resample_from) // g
        rs = filter.rational_resampler_ccf(interp, decim)
        head = tuple(head) + (rs,)
        if not args.quiet:
            print(
                f"重采样: {args.resample_from} -> {SAMP_RATE} "
                f"(interp={interp}, decim={decim})",
                flush=True,
            )

    fs = lora_sdr.frame_sync(
        int(CENTER_FREQ), BW, SF, IMPL_HEAD, [SYNC_WORD],
        OS_FACTOR, PREAMB_LEN,
    )
    fft = lora_sdr.fft_demod(False, True)
    gm = lora_sdr.gray_mapping(False)
    dei = lora_sdr.deinterleaver(False)
    hd = lora_sdr.hamming_dec(False)
    hdec = lora_sdr.header_decoder(
        IMPL_HEAD, CR, 255, HAS_CRC, 2, False,
    )
    dw = lora_sdr.dewhitening()
    crc = lora_sdr.crc_verif(0, False)

    sink = MeshPacketSink(
        pipeline, limit=args.limit, tb=tb, verbose=not args.quiet,
    )

    chain = list(head) + [fs, fft, gm, dei, hd, hdec, dw, crc, sink]
    tb.connect(*chain)
    tb.msg_connect((hdec, "frame_info"), (fs, "frame_info"))

    return tb, sink


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="LoRa 接收流图 + EMlink 帧解析 + MAVLink 打印",
    )
    src_group = parser.add_mutually_exclusive_group(required=True)
    src_group.add_argument("--file", help="int16 交错 IQ 文件")
    src_group.add_argument(
        "--udp", nargs="?", const=4242, type=int,
        help="从 UDP 收 int16 交错流（默认端口 4242）",
    )
    parser.add_argument("--format", choices=("i16", "u8", "cf32"), default="i16",
                        help="i16 = int16 交错（默认），u8 = rtl_sdr 的 8-bit 无符号交错，"
                             "cf32 = complex float32")
    parser.add_argument("--hex", action="store_true",
                        help="额外打印 payload hex")
    parser.add_argument("--limit", type=int, default=None,
                        help="收到 N 条 mesh 帧后停止")
    parser.add_argument("--quiet", action="store_true",
                        help="只打印解码结果，不打流图噪声")
    parser.add_argument(
        "--resample-from", type=int, default=None, dest="resample_from",
        help="源侧采样率；与 1.0 MS/s 不同时自动有理重采样"
             "（SDR++ network_sink 上限 192000 时用这个）",
    )
    parser.add_argument(
        "--udp-payload", type=int, default=12800, dest="udp_payload",
        help="每个 UDP 包的字节数，必须与 SDR++ network_sink 实际包长一致"
             "（实测 192000 Hz + stereo 时是 12800；对不上会丢掉大部分样本）",
    )
    args = parser.parse_args(argv)

    if args.limit is not None and args.limit < 1:
        parser.error("--limit 必须 >= 1")

    pipeline = MeshPipeline(show_hex=args.hex, quiet=args.quiet)
    tb, sink = build_flowgraph(args, pipeline)

    if not args.quiet:
        src_text = f"file={args.file}" if args.file else f"udp={args.udp}"
        print(
            f"接收链: {src_text} format={args.format} "
            f"samp_rate={SAMP_RATE} bw={BW} sf={SF} cr={CR} "
            f"os_factor={OS_FACTOR} ldro={LDRO} sync=0x{SYNC_WORD:02x}",
            flush=True,
        )

    try:
        tb.run()
    except KeyboardInterrupt:
        tb.stop()
        tb.wait()

    pipeline.reasm.tick(now_ms=time.monotonic() * 1000.0)
    st = pipeline.reasm.stats()
    st_final = pipeline.stats

    print(
        f"统计: 帧数={st_final.frames} 解帧失败={st_final.decode_fail} "
        f"MAVLink消息={st_final.mav_msgs} 重组完成={st_final.reassembled} "
        f"重组超时={st['timeout']}"
        + (f" LoRaCRC失败={st_final.crc_bad}" if st_final.crc_bad else "")
        + (f" 重组待完成={st['pending']}" if st["pending"] else ""),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
