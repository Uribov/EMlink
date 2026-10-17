"""CLI 命令队列：同一设备同一时刻只发一条，等回显或超时后再发下一条。

固件的 CLI 行缓冲很小，并发灌命令会导致回显与结果错位，所以这里串行化。
"""

import queue
import threading
import time


ECHO_TIMEOUT = 1.2
POLL_QUIET = 0.2


class CliClient:
    def __init__(self, write_func):
        self._write = write_func
        self._queue = queue.Queue()
        self._activity = threading.Event()
        self._expected = None
        self._closed = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def enqueue(self, text):
        text = (text or "").strip()
        if not text:
            return False, "命令为空"
        if "\xfd" in text or "\xfe" in text:
            return False, "命令不能包含 0xFD/0xFE（MAVLink 帧首字节）"
        self._queue.put(text)
        return True, ""

    def on_rx_line(self, line):
        """只有收到刚发出去那条命令的回显，才认为本命令完成。"""
        expected = self._expected
        if not expected:
            return
        text = (line or "").strip()
        if text.startswith("> "):
            text = text[2:].strip()
        if text == expected:
            self._activity.set()

    def clear(self):
        """清掉还没发出的命令（断开/重连时调用）。"""
        while True:
            try:
                self._queue.get_nowait()
            except queue.Empty:
                return

    def close(self):
        self._closed = True
        self._queue.put(None)

    def _run(self):
        while not self._closed:
            try:
                text = self._queue.get(timeout=POLL_QUIET)
            except queue.Empty:
                continue
            if text is None:
                break
            self._activity.clear()
            self._expected = text
            try:
                self._write((text + "\n").encode("utf-8"))
            except Exception:
                self._expected = None
                continue
            self._activity.wait(ECHO_TIMEOUT)
            self._expected = None
            # 给下一条命令留一点静默间隔，避免多行结果与下一条回显粘连。
            time.sleep(0.02)
