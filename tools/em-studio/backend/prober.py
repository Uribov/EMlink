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


_DIAG_RULES = (
    {
        "layer": "驱动层",
        "level": "err",
        "text": "驱动层：射频模块未检测到 —— 检查 E220 排线/供电，装好后 reset",
        "kind": "contains",
        "need": ("模块", "未检测到"),
    },
    {
        "layer": "驱动层",
        "level": "err",
        "text": "驱动层：射频未就绪",
        "kind": "ready_false",
    },
    {
        "layer": "中间层",
        "level": "err",
        "text": "中间层：加密认证失败 —— 两端密钥不一致",
        "kind": "count",
        "tokens": ("认证失败",),
    },
    {
        "layer": "中间层",
        "level": "err",
        "text": "中间层：两端协议版本不一致 —— 需刷同一版本固件",
        "kind": "count",
        "tokens": ("协议不一致",),
    },
    {
        "layer": "中间层",
        "level": "warn",
        "text": "中间层：重放丢弃计数非零 —— 检查时间窗/重放保护",
        "kind": "count",
        "tokens": ("重放丢弃",),
    },
    {
        "layer": "中间层",
        "level": "warn",
        "text": "中间层：出现环路或重复包（TTL/去重计数非零）",
        "kind": "count_any",
        "tokens": ("TTL丢弃", "去重淘汰"),
    },
    {
        "layer": "中间层",
        "level": "warn",
        "text": "中间层：发送背压丢包 —— 链路速率/队列容量不足",
        "kind": "count",
        "tokens": ("发送队列丢",),
    },
    {
        "layer": "应用层",
        "level": "err",
        "text": "应用层：sysid 与 node_id 同时冲突 —— 多节点重复配置",
        "kind": "count_all",
        "tokens": ("sysid撞号", "node_id撞号"),
    },
    {
        "layer": "应用层",
        "level": "err",
        "text": "应用层：sysid 撞号 —— 多节点用了同一 sysid",
        "kind": "count",
        "tokens": ("sysid撞号",),
    },
    {
        "layer": "应用层",
        "level": "err",
        "text": "应用层：node_id 撞号 —— 多节点用了同一 node_id",
        "kind": "count",
        "tokens": ("node_id撞号",),
    },
    {
        "layer": "系统",
        "level": "warn",
        "text": "系统：曾发生看门狗复位 —— 查看启动日志定位卡死模块",
        "kind": "count",
        "tokens": ("wdt复位",),
    },
    {
        "layer": "射频参数",
        "level": "warn",
        "text": "射频参数：ADR 未对齐（SF/带宽未协商一致）",
        "kind": "adr_false",
    },
)


def _first_number(text):
    match = _NUMBER.search(str(text or ""))
    if not match:
        return None
    try:
        return float(match.group(0))
    except ValueError:
        return None


def _item_text(item):
    label = str(item.get("label") or "")
    value = str(item.get("value") or "")
    return label, value, label + " " + value


def _count_positive(items, token):
    for item in items:
        label, value, _ = _item_text(item)
        if token not in label and token not in value:
            continue
        number = _first_number(value)
        if number is None:
            number = _first_number(label)
        if number is not None and number > 0:
            return True
    return False


def _match_rule(rule, items):
    kind = rule.get("kind")
    if kind == "contains":
        first, second = rule.get("need", ("", ""))
        return any(
            first in item_text and second in item_text
            for item_text in (_item_text(item)[2] for item in items)
        )
    if kind == "ready_false":
        for item in items:
            label, value, _ = _item_text(item)
            if "就绪" in label and "否" in value:
                return True
        return False
    if kind == "adr_false":
        adr_off = False
        for item in items:
            label, value, _ = _item_text(item)
            if "ADR 对齐" in label and "否" in value:
                adr_off = True
        if not adr_off:
            return False
        # 单机（没有任何邻接活动）时 ADR 无从对齐，属于正常状态，不该报警。
        return any(
            _count_positive(items, token)
            for token in ("信标", "多跳", "多锚点")
        )
    if kind == "count":
        return any(_count_positive(items, token) for token in rule.get("tokens", ()))
    if kind == "count_any":
        return any(_count_positive(items, token) for token in rule.get("tokens", ()))
    if kind == "count_all":
        return all(_count_positive(items, token) for token in rule.get("tokens", ()))
    return False


def diagnose(layers):
    """把 status 解析结果翻译成人话结论。

    返回 [{"layer": "驱动层", "level": "err|warn", "text": "人话结论"}]。
    err 在前、warn 在后；同一层只保留规则表里最靠前（最具体）的一条。
    没有命中任何规则时返回空列表，由前端隐藏归因行。
    """
    items = []
    for layer in layers or []:
        if not isinstance(layer, dict):
            continue
        for item in layer.get("items") or []:
            if isinstance(item, dict):
                items.append(item)

    verdicts = []
    seen_layers = set()
    for rule in _DIAG_RULES:
        if rule["layer"] in seen_layers:
            continue
        if not _match_rule(rule, items):
            continue
        seen_layers.add(rule["layer"])
        verdicts.append({
            "layer": rule["layer"],
            "level": rule["level"],
            "text": rule["text"],
        })

    verdicts.sort(key=lambda item: 0 if item.get("level") == "err" else 1)
    return verdicts


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
