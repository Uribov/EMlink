"""调用系统 esptool.py 烧录 ESP32-S3 固件，并实时解析进度。"""

import os
import queue
import re
import shutil
import subprocess
import threading
import time

APP_OFFSET = 0x10000
FLASH_MODE = "qio"
FLASH_SIZE = "4MB"
CHIP = "esp32s3"

_PROGRESS_RE = re.compile(r"(\d+)\s*%")


def find_esptool():
    """在 PATH 里找 esptool.py；ESPTOOL 环境变量可覆盖。找不到返回 None。"""
    override = os.environ.get("ESPTOOL", "").strip()
    if override:
        if os.path.isfile(override) and os.access(override, os.X_OK):
            return override
        return shutil.which(override)

    return shutil.which("esptool.py")


def _base_command(esptool, port, baud):
    return [
        esptool,
        "--chip",
        CHIP,
        "--port",
        str(port),
        "--baud",
        str(baud),
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
    ]


def _build_commands(esptool, port, bin_path, erase, baud, offset):
    base = _base_command(esptool, port, baud)
    commands = []
    if erase:
        commands.append(base + ["erase_flash"])
        write_offset = 0x0
    else:
        write_offset = offset
    commands.append(
        base
        + [
            "write_flash",
            "--flash_mode",
            FLASH_MODE,
            "--flash_size",
            FLASH_SIZE,
            hex(write_offset),
            bin_path,
        ]
    )
    return commands


def _terminate(proc):
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        try:
            proc.kill()
            proc.wait(timeout=2)
        except Exception:
            pass


def _first_error(lines, fallback):
    for line in reversed(lines):
        text = (line or "").strip()
        if not text:
            continue
        low = text.lower()
        if any(word in low for word in ("error", "failed", "fatal", "exception")):
            return text.splitlines()[0]
    for line in reversed(lines):
        text = (line or "").strip()
        if text:
            return text.splitlines()[0]
    return fallback


def _run_command(cmd, *, deadline, timeout, on_progress, log_lines):
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    lines = queue.Queue()

    def reader():
        try:
            for line in proc.stdout:
                lines.put(line)
        finally:
            lines.put(None)

    thread = threading.Thread(target=reader, name="esptool-rx", daemon=True)
    thread.start()

    timed_out = False
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                _terminate(proc)
                break
            try:
                line = lines.get(timeout=min(remaining, 0.2))
            except queue.Empty:
                continue
            if line is None:
                break
            log_lines.append(line)
            if on_progress:
                for match in _PROGRESS_RE.finditer(line):
                    percent = max(0, min(100, int(match.group(1))))
                    on_progress(percent)
    finally:
        try:
            proc.stdout.close()
        except Exception:
            pass

    if timed_out:
        return f"烧录超时（超过 {timeout} 秒）"

    try:
        code = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        _terminate(proc)
        return f"烧录超时（超过 {timeout} 秒）"

    if code == 0:
        return ""
    return _first_error(log_lines, f"esptool 退出码 {code}")


def flash(
    port,
    bin_path,
    *,
    erase=False,
    baud=460800,
    offset=APP_OFFSET,
    on_progress=None,
    timeout=180,
):
    """烧录并返回 {"ok": bool, "message": str, "log": str}。"""
    esptool = find_esptool()
    if not esptool:
        return {
            "ok": False,
            "message": "找不到 esptool.py（请确认已安装，或用 ESPTOOL 环境变量指定）",
            "log": "",
        }

    bin_path = os.path.abspath(str(bin_path or ""))
    if not os.path.isfile(bin_path):
        return {"ok": False, "message": f"固件文件不存在：{bin_path}", "log": ""}
    if offset != APP_OFFSET and not erase:
        return {"ok": False, "message": "只写 app 时偏移必须是 0x10000", "log": ""}

    commands = _build_commands(esptool, port, bin_path, erase, baud, offset)
    deadline = time.monotonic() + max(1, int(timeout))
    log_lines = []

    for command in commands:
        error = _run_command(
            command,
            deadline=deadline,
            timeout=timeout,
            on_progress=on_progress,
            log_lines=log_lines,
        )
        if error:
            return {
                "ok": False,
                "message": error,
                "log": "".join(log_lines),
            }

    if on_progress:
        on_progress(100)
    return {"ok": True, "message": "烧录完成", "log": "".join(log_lines)}
