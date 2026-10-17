"""诊断结果结构化：把板子 `status` 的三层文本解析成卡片数据。

真实输出格式以 firmware/src/app/cli.cpp::cmd_status 为准：
    --- 驱动层 ---
    射频: 就绪=是  模块=在线  请求超时=0
    外设: 收包队列丢=0  射频请求丢=0  UART丢=0  USB丢=0
    --- 中间层 ---
    链路: 占空比超限放行=0  认证失败=0  ...
    ...

条目之间用两个及以上空格分隔（`ADR 对齐=是` 这种键内部只有一个空格，
所以不能按任意空白切）。status 走静默查询，避免轮询刷屏。
"""

import re


_SEPARATOR = re.compile(r"^\s*-{2,}\s*(.+?)\s*-{2,}\s*$")
_COLON = re.compile(r"[:：]")
_PAIR_GAP = re.compile(r"\s{2,}")
_NEG_ERR = re.compile(r"err\s*=\s*-\s*\d+", re.IGNORECASE)
_NUMBER = re.compile(r"[-+]?\d+(?:\.\d+)?")
_ERR_WORDS = ("未检测到", "初始化失败", "失败", "否")
_WARN_WORDS = (
    "丢", "掉", "失败", "超限", "超时", "过期", "淘汰", "撞号",
    "降级", "不一致", "失步", "重放", "重传", "拒绝",
)
_PARAM_WHITELIST = frozenset({
    "heap", "freq", "sf", "bw", "cr", "power", "ttl", "retries",
    "beacon_s", "failsafe_s", "ack_timeout_ms", "uart_baud", "syncword",
    "node_id", "sysid", "auto_wifi_s", "name",
})


def _clean_lines(lines):
    cleaned = []
    for line in lines or []:
        text = str(line or "").replace("\r", "").strip()
        if text.startswith("> "):
            text = text[2:].strip()
        cleaned.append(text)
    return cleaned


def _level(label, value):
    """按字段语义给值分色。

    err：值里出现故障词或负 err；warn：仅计数类字段（label 含关键词）且
    值中任一数字 > 0；参数白名单即使数值很大也保持 ok。
    """
    text = str(value or "").strip()
    if any(word in text for word in _ERR_WORDS) or _NEG_ERR.search(text):
        return "err"

    key = str(label or "").strip().lower()
    if key in _PARAM_WHITELIST:
        return "ok"
    if not any(word in key for word in _WARN_WORDS):
        return "ok"

    numbers = [float(match) for match in _NUMBER.findall(text)]
    return "warn" if any(number > 0 for number in numbers) else "ok"


def _pairs(text):
    """把一行里的 key=value 拆成 (key, value) 列表；拆不开的整段原样保留。"""
    result = []
    for part in _PAIR_GAP.split(text.strip()):
        part = part.strip()
        if not part:
            continue
        key, sep, value = part.partition("=")
        if sep and key.strip():
            result.append((key.strip(), value.strip()))
        else:
            result.append((part, ""))
    return result


def _line_items(text):
    """一行 → 若干条目：`标签: 值` 或 `key=value`（一行多个 key=value 拆多条）。"""
    text = text.strip()
    if not text:
        return []
    match = _COLON.search(text)
    if match:
        head = text[:match.start()].strip()
        body = text[match.end():].strip()
        if "=" in body:
            return _pairs(body)
        return [(head, body)] if head else []
    if "=" in text:
        return _pairs(text)
    return [(text, "")]


def parse_status(lines):
    """把 status 文本解析成 layers；没有分隔段时返回空列表。"""
    layers = []
    current = None
    for text in _clean_lines(lines):
        separator = _SEPARATOR.match(text)
        if separator:
            current = {"name": separator.group(1).strip(), "items": []}
            layers.append(current)
            continue
        if not current:
            continue
        if not text or text.startswith("#") or text.startswith("==="):
            continue
        if not re.search(r"[0-9A-Za-z\u4e00-\u9fff]", text):
            continue
        for label, value in _line_items(text):
            if not label:
                continue
            current["items"].append({
                "label": label,
                "value": value,
                "level": _level(label, value),
            })
    return layers


def read_status(link):
    """静默发 status 并解析。返回 (data, error)。

    data = {"layers": [{"name": ..., "items": [{"label", "value", "level"}]}],
            "raw": "..."}；解析不到任何层时 data=None，由前端保留上一版卡片。
    """
    if link is None:
        return None, "设备不存在"
    if getattr(link, "status", "") != "connected":
        return None, "设备未连接"
    cli = getattr(link, "cli", None)
    if cli is None or not hasattr(cli, "query"):
        return None, "命令队列不可用"

    try:
        ok, lines, error = cli.query(
            "status", timeout=1.5, quiet=0.25, silent=True
        )
    except Exception as exc:
        return None, f"读状态失败：{exc}"

    cleaned = _clean_lines(lines)
    layers = parse_status(cleaned)
    if not layers:
        return None, error or "板子没有返回 status"

    data = {"layers": layers, "raw": "\n".join(cleaned).strip()}
    return data, ("" if ok else (error or ""))
