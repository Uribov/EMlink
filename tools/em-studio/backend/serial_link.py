"""每设备一个串口链路实例：打开/关闭、读线程、轮询缓冲。

读线程是 daemon，只负责把字节喂给该设备的 dispatcher；
前端通过 poll() 取走自上次以来的增量文本。
"""

import threading

import serial

from . import mav_raw
from .cli_client import CliClient
from .dispatcher import Dispatcher


def _to_int(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return None


def parse_led_line(line):
    """解析板子主动推送的灯态行，字段缺失或非法时对应值为 None。"""
    result = {"mode": None, "r": None, "g": None, "b": None, "half": None}
    text = (line or "").strip()
    if text.startswith("#led"):
        text = text[4:].strip()
    for token in text.split():
        key, sep, value = token.partition("=")
        if not sep:
            continue
        key = key.strip()
        value = value.strip()
        if key == "mode":
            result["mode"] = value or None
        elif key == "color":
            parts = value.split(",")
            if len(parts) == 3:
                result["r"] = _to_int(parts[0])
                result["g"] = _to_int(parts[1])
                result["b"] = _to_int(parts[2])
        elif key == "half":
            result["half"] = _to_int(value)
    return result


class SerialLink:
    def __init__(self, device_id, port, baud=115200):
        self.device_id = device_id
        self.port = port
        self.baud = int(baud or 115200)

        self._serial = None
        self._running = False
        self._read_thread = None
        self._write_lock = threading.Lock()
        self._lines_lock = threading.Lock()
        self._lines = []
        self._mav_lock = threading.Lock()
        self._mav_buffer = b""
        self._mav_frames = []
        self._status = "closed"
        self._note = ""
        self.led_state = None

        self.dispatcher = Dispatcher(self._handle_line)
        self.cli = CliClient(self._write)

    @property
    def status(self):
        return self._status

    @property
    def mode(self):
        return self.dispatcher.mode

    def set_mode(self, mode):
        if not self.dispatcher.set_mode(mode):
            return False
        with self._mav_lock:
            self._mav_buffer = b""
            self._mav_frames = []
        return True

    def open(self):
        if self._status == "connected" and self._serial is not None:
            return True, ""

        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.05)
        except Exception as exc:
            message = self._friendly_open_error(exc)
            self._status = "error"
            self._note = message
            return False, message

        with self._write_lock:
            self._serial = ser
        self.cli.clear()
        self.dispatcher.reset()
        with self._mav_lock:
            self._mav_buffer = b""
            self._mav_frames = []
        self.led_state = None
        self._status = "connected"
        self._note = ""
        self._running = True
        self._read_thread = threading.Thread(
            target=self._read_loop, name=f"em-{self.device_id}-rx", daemon=True
        )
        self._read_thread.start()
        return True, ""

    def close(self):
        self._running = False
        self.cli.clear()
        self._close_serial()
        thread = self._read_thread
        if thread is not None and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout=0.3)
        self._read_thread = None
        if self._status != "error":
            self._status = "closed"
            self._note = ""

    def close_permanently(self):
        self.close()
        self.cli.close()

    def send(self, text):
        if self._status != "connected":
            return False, "设备未连接"
        return self.cli.enqueue(text)

    def send_bytes(self, payload):
        if self._status != "connected":
            return False, "设备未连接"
        try:
            self._write(payload)
        except Exception as exc:
            return False, f"发送失败：{exc}"
        return True, ""

    def poll(self):
        with self._lines_lock:
            lines = self._lines
            self._lines = []
        return {"lines": lines, "status": self._status, "note": self._note}

    def poll_mav(self):
        with self._mav_lock:
            frames = self._mav_frames
            self._mav_frames = []
        return {
            "frames": [mav_raw.frame_to_json(frame) for frame in frames],
            "status": self._status,
            "note": self._note,
            "mode": self.dispatcher.mode,
        }

    def _read_loop(self):
        while self._running:
            ser = self._serial
            if ser is None:
                break
            try:
                waiting = ser.in_waiting
                data = ser.read(waiting or 1)
            except Exception as exc:
                if self._running:
                    self._running = False
                    self._status = "error"
                    self._note = f"串口读取失败：{exc}"
                    self._append_line(f"[串口读取失败：{exc}]")
                    self._close_serial()
                break
            if data:
                if self.dispatcher.mode == "mavlink":
                    self._feed_mav(data)
                else:
                    self.dispatcher.feed(data)

    def _feed_mav(self, data):
        with self._mav_lock:
            frames, self._mav_buffer = mav_raw.parse_frames(self._mav_buffer + data)
            if frames:
                self._mav_frames.extend(frames)
                overflow = len(self._mav_frames) - 1000
                if overflow > 0:
                    del self._mav_frames[:overflow]

    def _write(self, payload):
        with self._write_lock:
            ser = self._serial
            if ser is None or not ser.is_open:
                raise RuntimeError("串口未连接")
            ser.write(payload)

    def _close_serial(self):
        with self._write_lock:
            ser = self._serial
            self._serial = None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    def _handle_line(self, line):
        # 板子的灯态推送（F8a）：自己消化，不进诊断台输出，也不参与命令响应配对。
        if line.startswith("#led "):
            self.led_state = parse_led_line(line)
            return
        self.cli.on_rx_line(line)
        # 静默查询（轮询 status）：回显与输出只交给解析器，不进前端输出队列。
        if self.cli.is_silent():
            return
        self._append_line(line)

    def _append_line(self, line):
        with self._lines_lock:
            self._lines.append(line)

    def _friendly_open_error(self, exc):
        raw = str(exc)
        low = raw.lower()
        if (
            "busy" in low
            or "resource busy" in low
            or "access is denied" in low
            or "permission denied" in low
            or "拒绝访问" in raw
            or "被占用" in raw
        ):
            return f"无法打开 {self.port}：端口被占用或没有权限（{raw}）"
        if (
            "no such file" in low
            or "not found" in low
            or "filenotfound" in low
            or "找不到" in raw
        ):
            return f"无法打开 {self.port}：端口不存在或设备已拔出（{raw}）"
        return f"无法打开 {self.port}：{raw}"
