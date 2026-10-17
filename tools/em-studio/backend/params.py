"""参数区后端：读配置、单键写配置、保存/重启。

所有命令都走 SerialLink 已有的 CliClient 队列（串行 + 等回显），
本模块只负责拼命令与解析板子回的文本。
"""

import re


# CLI `get`（不带键）实际会打印这些键（见 firmware/src/app/cli.cpp::cmd_get_all）
GET_ALL_KEYS = {
    "role", "node_id", "sysid", "freq", "sf", "bw", "cr", "power",
    "syncword", "uart_baud", "ttl", "ack_timeout_ms", "retries",
    "beacon_s", "failsafe_s", "name",
}
PARSE_KEYS = GET_ALL_KEYS | {"auto_wifi_s"}

# 参数区需要回填的字段；拿不到的保持 None（前端保留默认值）
UI_KEYS = (
    "role", "node_id", "freq", "sf", "bw", "cr", "power",
    "auto_wifi_s", "crypto",
)

ALLOWED_SET_KEYS = {
    "role", "node_id", "sysid", "freq", "sf", "bw", "cr", "power",
    "syncword", "uart_baud", "ttl", "ack_timeout_ms", "retries",
    "beacon_s", "failsafe_s", "auto_wifi_s", "name",
}

_KV_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$")
_EQUALS_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$")
_HEX32 = re.compile(r"^[0-9a-fA-F]{32}$")


def _clean_lines(lines):
    cleaned = []
    for line in lines or []:
        text = str(line or "").strip()
        if text.startswith("> "):
            text = text[2:].strip()
        if text:
            cleaned.append(text)
    return cleaned


def _parse_values(lines):
    """从 get 输出里取已知键；不认识的行、回显、提示行一律忽略。"""
    values = {}
    for text in _clean_lines(lines):
        match = _EQUALS_LINE.match(text)
        if match:
            key, value = match.group(1), match.group(2).strip()
        else:
            match = _KV_LINE.match(text)
            if not match:
                continue
            key, value = match.group(1), match.group(2).strip()
        if key in PARSE_KEYS:
            values[key] = value
    return values


def _parse_crypto(lines):
    for text in _clean_lines(lines):
        if "加密：开" in text:
            return "on"
        if "加密：关" in text:
            return "off"
    return None


def _query(link, command, timeout=2.0, quiet=0.25):
    if link is None:
        return False, [], "设备不存在"
    if getattr(link, "status", "") != "connected":
        return False, [], "设备未连接"
    cli = getattr(link, "cli", None)
    if cli is None or not hasattr(cli, "query"):
        return False, [], "命令队列不可用"
    try:
        return cli.query(command, timeout=timeout, quiet=quiet)
    except Exception as exc:
        return False, [], f"命令执行失败：{exc}"


def _board_text(lines, error):
    text = "\n".join(_clean_lines(lines))
    if text:
        return text
    return error or "板子没有返回结果"


def read_config(link):
    """读回参数区所需配置；返回 (data, error_note)。

    超时或部分解析失败时仍然返回已经读到的值，拿不到的键为 None。
    """
    data = {key: None for key in UI_KEYS}
    errors = []

    ok, lines, error = _query(link, "get", timeout=2.2, quiet=0.3)
    parsed = _parse_values(lines)
    for key in UI_KEYS:
        if key in parsed:
            data[key] = parsed[key]
    if not ok and error:
        errors.append(error)

    # `get`（无键）不含 auto_wifi_s 与加密状态，需要各补一条命令。
    if any(value is not None for key, value in data.items() if key not in ("crypto",)):
        ok, lines, error = _query(link, "get auto_wifi_s", timeout=1.5, quiet=0.25)
        if data["auto_wifi_s"] is None:
            data["auto_wifi_s"] = _parse_values(lines).get("auto_wifi_s")
        if not ok and error:
            errors.append(error)

        ok, lines, error = _query(link, "crypto", timeout=1.5, quiet=0.25)
        data["crypto"] = _parse_crypto(lines)
        if not ok and error:
            errors.append(error)
    else:
        errors.append("未解析到配置（板子可能未响应 get）")

    note = "；".join(dict.fromkeys(item for item in errors if item))
    return data, note


def set_config(link, key, value):
    """改动即写内存（set，或 crypto 命令）；返回 (ok, board_error)。"""
    key = str(key or "").strip()
    value = str(value or "").strip()

    if not key:
        return False, "键名为空"

    if key in ("crypto", "crypto_key"):
        if value.lower() in ("off", "0", "关"):
            command = "crypto off"
        else:
            if not _HEX32.match(value):
                return False, "加密密钥必须是 32 位 hex，或输入 off 关闭"
            command = "crypto " + value.lower()
    else:
        if key not in ALLOWED_SET_KEYS:
            return False, f"参数区不支持这个键：{key}"
        if not value:
            return False, "值为空"
        command = f"set {key} {value}"

    ok, lines, error = _query(link, command, timeout=2.0, quiet=0.25)
    text = "\n".join(_clean_lines(lines))

    if "设置失败" in text or "用法：" in text or "用法:" in text:
        return False, _board_text(lines, error)
    if any(marker in text for marker in ("已设置", "密钥已设置", "加密已关闭", "角色已设为")):
        return True, ""
    if text:
        return False, text
    return False, error or "板子没有返回结果"


def save_config(link, reboot=False):
    """先 save 写 NVS；reboot=True 时再排一条 reset。返回 (saved, rebooting, error)。"""
    ok, lines, error = _query(link, "save", timeout=2.0, quiet=0.25)
    text = "\n".join(_clean_lines(lines))
    if "已保存" not in text:
        return False, False, _board_text(lines, error)

    if not reboot:
        return True, False, ""

    cli = getattr(link, "cli", None)
    if cli is None:
        return True, False, "已保存，但命令队列不可用，无法重启"
    sent, send_error = cli.enqueue("reset")
    return True, bool(sent), "" if sent else (send_error or "重启命令发送失败")
