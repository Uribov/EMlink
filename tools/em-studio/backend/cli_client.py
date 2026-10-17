"""CLI 命令队列：同一设备同一时刻只发一条，等回显或超时后再发下一条。

固件的 CLI 行缓冲很小，并发灌命令会导致回显与结果错位，所以这里串行化。
"""

import queue
import threading
import time


ECHO_TIMEOUT = 1.2
POLL_QUIET = 0.2


class _Query:
    """一条需要收集输出的命令（如 get / set / save）。"""

    def __init__(self, text, timeout, quiet):
        self.text = text
        self.timeout = timeout
        self.quiet = quiet
        self.lines = []
        self.error = ""
        self.started = False
        self.last_line_at = 0.0
        self.done = threading.Event()


class CliClient:
    def __init__(self, write_func):
        self._write = write_func
        self._queue = queue.Queue()
        self._activity = threading.Event()
        self._expected = None
        self._capture = None
        self._closed = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    @staticmethod
    def _check(text):
        text = (text or "").strip()
        if not text:
            return None, "命令为空"
        if "\xfd" in text or "\xfe" in text:
            return None, "命令不能包含 0xFD/0xFE（MAVLink 帧首字节）"
        if "\r" in text or "\n" in text:
            return None, "命令不能包含换行"
        return text, ""

    def enqueue(self, text):
        text, error = self._check(text)
        if text is None:
            return False, error
        self._queue.put(text)
        return True, ""

    def query(self, text, timeout=2.0, quiet=0.25):
        """串行执行一条命令，等回显后收集紧随其后的输出行。

        返回 (ok, lines, error)。超时也会把已经收到的行带回来，调用方可
        用部分结果做健壮解析。这条命令仍然走同一个队列，不会和普通发送并发。
        """
        text, error = self._check(text)
        if text is None:
            return False, [], error
        if self._closed:
            return False, [], "命令队列已关闭"
        job = _Query(text, max(0.2, float(timeout)), max(0.05, float(quiet)))
        self._queue.put(job)
        if not job.done.wait(job.timeout * 2 + job.quiet + 5.0):
            return False, list(job.lines), "等待板子响应超时"
        return not job.error, list(job.lines), job.error

    def on_rx_line(self, line):
        """匹配回显；回显之后的行进入当前 query 的输出缓冲。"""
        text = (line or "").strip()
        if text.startswith("> "):
            text = text[2:].strip()

        expected = self._expected
        if expected and text == expected:
            self._activity.set()
            capture = self._capture
            if capture is not None:
                capture.started = True
                capture.last_line_at = time.monotonic()
            return

        capture = self._capture
        if capture is not None and capture.started:
            capture.lines.append(text)
            capture.last_line_at = time.monotonic()

    def clear(self):
        """清掉还没发出的命令（断开/重连时调用）。"""
        while True:
            try:
                item = self._queue.get_nowait()
            except queue.Empty:
                return
            if isinstance(item, _Query):
                item.error = "命令已取消"
                item.done.set()

    def close(self):
        self._closed = True
        self._queue.put(None)

    def _run(self):
        while not self._closed:
            try:
                item = self._queue.get(timeout=POLL_QUIET)
            except queue.Empty:
                continue
            if item is None:
                break
            if isinstance(item, _Query):
                self._run_query(item)
            else:
                self._run_text(item)

    def _run_text(self, text):
        self._activity.clear()
        self._expected = text
        try:
            self._write((text + "\n").encode("utf-8"))
        except Exception:
            self._expected = None
            return
        self._activity.wait(ECHO_TIMEOUT)
        self._expected = None
        # 给下一条命令留一点静默间隔，避免多行结果与下一条回显粘连。
        time.sleep(0.02)

    def _run_query(self, job):
        self._activity.clear()
        self._expected = job.text
        self._capture = job
        job.started = False
        try:
            self._write((job.text + "\n").encode("utf-8"))
        except Exception as exc:
            job.error = f"命令发送失败：{exc}"
            self._finish_query(job)
            return
        if not self._activity.wait(job.timeout):
            job.error = "等待命令回显超时"
            self._finish_query(job)
            return

        deadline = time.monotonic() + job.timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                if not job.error:
                    job.error = "等待板子响应超时"
                break
            idle = time.monotonic() - job.last_line_at
            if idle >= job.quiet:
                break
            time.sleep(min(0.02, remaining))
        self._finish_query(job)

    def _finish_query(self, job):
        self._expected = None
        self._capture = None
        job.done.set()
        time.sleep(0.02)
