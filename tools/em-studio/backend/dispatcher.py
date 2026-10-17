"""模式分发器：按当前模式把串口字节交给 CLI 或 MAVLink 解析器。

本版（D-1）只实现 CLI 文本行解析；MAVLink 模式为占位，收到字节直接丢弃。
后续 D-3 在 mavlink 分支接入帧解析，并复用 reset() 完成切模式时的状态复位。
"""

import codecs


def _printable(text):
    """过滤不可打印字符，保留换行与制表符。"""
    return "".join(ch for ch in text if ch in "\n\t" or ch.isprintable())


class Dispatcher:
    def __init__(self, on_line):
        self._on_line = on_line
        self._mode = "cli"
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._buffer = ""

    @property
    def mode(self):
        return self._mode

    def reset(self):
        """丢弃可能被切换截断的半行/半帧。"""
        self._decoder.reset()
        self._buffer = ""

    def set_mode(self, mode):
        if mode not in ("cli", "mavlink"):
            return False
        if mode != self._mode:
            self._mode = mode
            self.reset()
        return True

    def feed(self, data):
        if not data:
            return
        if self._mode == "mavlink":
            # D-3 之前：MAVLink 字节直接丢弃，避免污染 CLI 视图。
            return
        text = self._decoder.decode(data)
        if not text:
            return
        self._buffer += _printable(text)
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            self._on_line(line.rstrip("\r"))
