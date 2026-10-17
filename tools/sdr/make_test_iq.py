#!/usr/bin/env python3
"""造一段含 EMlink Mesh 帧的 LoRa IQ 测试文件（离线端到端用）。

发射链与 gr-lora_sdr 的 tx_rx_functionality_check.py 一致，只把数据源换成
带 packet_len tag 的字节流，因此一个标签 = 一条独立的 LoRa packet = 一条
EMlink Mesh 帧。

用法（必须用带 gnuradio 的解释器，见 S2 任务书 §3）：
    /usr/bin/python3 tools/sdr/make_test_iq.py --out tools/sdr/test_pattern.i16
"""

from __future__ import annotations

import argparse
import os
import sys

from emlink_frame import (
    FRAME_DATA,
    PROTO_MAVLINK,
    encode_frame,
)

# ---- 空口参数（必须与 lora_mesh_live.py 一致） ----
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


def _mavlink():
    from pymavlink.dialects.v20 import ardupilotmega as m2
    return m2


def build_messages():
    """返回 (HEARTBEAT 字节, GLOBAL_POSITION_INT 字节, 200B LOGGING_DATA 字节)。"""
    m2 = _mavlink()
    # srcSystem/srcComponent 显式设为 1/1，便于验收时看到 sys=1 comp=1。
    mav = m2.MAVLink(None, srcSystem=1, srcComponent=1)

    heartbeat = m2.MAVLink_heartbeat_message(
        type=2, autopilot=12, base_mode=0, custom_mode=0,
        system_status=3, mavlink_version=3,
    ).pack(mav)
    if len(heartbeat) != 21 or heartbeat[0] != 0xFD:
        raise RuntimeError(f"HEARTBEAT 打包异常: len={len(heartbeat)}")

    global_pos = m2.MAVLink_global_position_int_message(
        time_boot_ms=1000,
        lat=399000000,
        lon=1164000000,
        alt=120000,
        relative_alt=100000,
        vx=0,
        vy=0,
        vz=0,
        hdg=9000,
    ).pack(mav)
    if global_pos[0] != 0xFD:
        raise RuntimeError("GLOBAL_POSITION_INT 打包异常")

    # 构造一条恰好 200 字节的合法 MAVLink2 消息：
    #   LOGGING_DATA = 10B 头 + 188B payload + 2B CRC。
    # MAVLink2 会裁掉 payload 末尾的 0，故把 data[181] 置为非零，
    # 使 payload 长度停在 188。
    data = bytearray(249)
    data[181] = 0x41
    big = m2.MAVLink_logging_data_message(
        target_system=1,
        target_component=1,
        sequence=7,
        length=181,
        first_message_offset=0,
        data=bytes(data),
    ).pack(mav)
    if len(big) != 200 or big[0] != 0xFD:
        raise RuntimeError(f"200B LOGGING_DATA 打包异常: len={len(big)}")

    return heartbeat, global_pos, big


def build_frames(rounds: int = 1):
    """按任务书 §5.3 造 4 条 mesh 帧（单帧心跳、单帧定位、2 片分片）。"""
    heartbeat, global_pos, big = build_messages()
    frag0, frag1 = big[:175], big[175:]
    if len(frag0) != 175 or len(frag1) != 25:
        raise RuntimeError("分片长度异常")

    frames = []
    for r in range(rounds):
        seq = 42 + r * 10
        common = dict(
            type=FRAME_DATA,
            qos=1,
            ack_req=False,
            enc=False,
            proto=PROTO_MAVLINK,
            src_id=3,
            dst_id=0,
            ttl=4,
            relay_id=2,
            orig_sysid=1,
            net_id=0,
            enc_epoch=7,
        )

        frames.append(encode_frame(
            **common, seq=seq, payload=heartbeat,
        ))
        frames.append(encode_frame(
            **common, seq=seq + 1, payload=global_pos,
        ))
        for index, part in enumerate((frag0, frag1)):
            frames.append(encode_frame(
                **common,
                seq=seq + 2 + index,
                frag=True,
                frag_msg_id=9,
                frag_index=index,
                frag_total=2,
                frag_seq=123 + r,
                payload=part,
            ))
    return frames


def build_flowgraph(frame_bytes_list, out_path: str, out_format: str, quiet: bool):
    import pmt
    from gnuradio import blocks, gr
    from gnuradio import lora_sdr

    data = b"".join(frame_bytes_list)
    tags = []
    off = 0
    for fb in frame_bytes_list:
        tags.append(gr.tag_utils.python_to_tag((
            off,
            pmt.intern("packet_len"),
            pmt.from_long(len(fb)),
            pmt.intern("emlink"),
        )))
        off += len(fb)

    tb = gr.top_block("emlink_make_test_iq")
    src = blocks.vector_source_b(list(data), False, 1, tags)

    wh = lora_sdr.whitening(False, True, ",", "packet_len")
    hdr = lora_sdr.header(IMPL_HEAD, HAS_CRC, CR)
    crc = lora_sdr.add_crc(HAS_CRC)
    ham = lora_sdr.hamming_enc(CR, SF)
    inter = lora_sdr.interleaver(CR, SF, LDRO, BW)
    gdemap = lora_sdr.gray_demap(SF)
    mod = lora_sdr.modulate(
        SF, SAMP_RATE, BW, [SYNC_WORD],
        int(20 * 2 ** SF * SAMP_RATE / BW), PREAMB_LEN,
    )
    mod.set_min_output_buffer(10_000_000)

    if not quiet:
        print(
            f"发射链: samp_rate={SAMP_RATE} bw={BW} sf={SF} cr={CR} "
            f"os_factor={OS_FACTOR} ldro={LDRO} sync=0x{SYNC_WORD:02x}",
            flush=True,
        )

    if out_format == "i16":
        to_i16 = blocks.complex_to_interleaved_short(False, float(32768))
        sink = blocks.file_sink(2, out_path, False)
        tb.connect(src, wh, hdr, crc, ham, inter, gdemap, mod, to_i16, sink)
    else:
        sink = blocks.file_sink(gr.sizeof_gr_complex, out_path, False)
        tb.connect(src, wh, hdr, crc, ham, inter, gdemap, mod, sink)

    return tb, sink


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="生成含 EMlink Mesh 帧的 LoRa IQ 测试文件",
    )
    parser.add_argument("--out", default="tools/sdr/test_pattern.i16",
                        help="输出 IQ 文件（默认 tools/sdr/test_pattern.i16）")
    parser.add_argument("--format", choices=("i16", "cf32"), default="i16",
                        help="i16 = int16 交错（默认），cf32 = complex float32")
    parser.add_argument("--frames", type=int, default=1,
                        help="重复造多少轮（默认 1 轮 = 4 条 mesh 帧）")
    parser.add_argument("--quiet", action="store_true", help="不打印进度")
    args = parser.parse_args(argv)

    if args.frames < 1:
        parser.error("--frames 必须 >= 1")

    frames = build_frames(args.frames)
    tb, sink = build_flowgraph(frames, args.out, args.format, args.quiet)

    try:
        tb.run()
    finally:
        tb.stop()
        tb.wait()
        sink.close()

    size = os.path.getsize(args.out)
    if args.format == "i16":
        samples = size // 2
    else:
        samples = size // 8

    print(
        f"帧数={len(frames)} 总样本数={samples} "
        f"文件={args.out} 大小={size} 字节",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
