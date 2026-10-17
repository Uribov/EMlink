#!/usr/bin/env python3
# ============================================================
# verify_webui.py —— 不接板子验证 Web 管理页
#
# 从固件 src/service/webui.h 里抠出各段 C 字符串，按 ota.cpp 的拼装顺序
# 还原成真实 HTML，做结构自检，并把结果写成可直接用浏览器打开的 .html。
#
# 用法：  python3 design/verify_webui.py
# 产物：  design/webui-render.html       （主页，模拟配置：节点01/air/加密开/915/SF7/125/CR5/20dBm）
#         design/webui-done-render.html  （回执页）
# 退出码：0 = 全部通过；1 = 有问题（页面尺寸超预留 / 标签不闭合 / 关键元素缺失）
#
# 为什么需要它：页面字符串是编译进固件的，改完只靠 pio run 只能证明「能编译」，
# 证明不了「页面是对的」。这个脚本补上后半截。
# ============================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "firmware/src/service/webui.h")
OTA = os.path.join(ROOT, "firmware/src/service/ota.cpp")
OUT_MAIN = os.path.join(ROOT, "design/webui-render.html")
OUT_DONE = os.path.join(ROOT, "design/webui-done-render.html")

VOID = {"meta", "br", "hr", "img", "input", "link", "area", "base", "col",
        "embed", "source", "track", "wbr", "path", "circle", "rect", "stop", "use"}


def literals(block: str) -> str:
    """把一段 C 表达式里的字符串字面量拼起来（处理 \\" 与 \\\\，并展开 WEBUI_VER）。"""
    out, i = [], 0
    while i < len(block):
        c = block[i]
        if c == '"':
            i += 1
            while i < len(block):
                if block[i] == '\\':
                    nxt = block[i + 1]
                    if nxt == '"':
                        out.append('"'); i += 2; continue
                    if nxt == '\\':
                        out.append('\\'); i += 2; continue
                    out.append(block[i]); i += 1; continue
                if block[i] == '"':
                    i += 1; break
                out.append(block[i]); i += 1
        elif c.isalpha() or c == '_':
            j = i
            while j < len(block) and (block[j].isalnum() or block[j] == '_'):
                j += 1
            if block[i:j] == "WEBUI_VER":
                out.append("v1.0")
            i = j
        else:
            i += 1
    return ''.join(out)


def extract(src: str) -> dict:
    """取出所有 static const char WUI_x[] PROGMEM = "..."; 的运行时内容。
    注意：不能在 ';' 上简单截断——CSS 里到处是分号，必须按「是否在字符串内」扫描。"""
    segs = {}
    for m in re.finditer(r'static const char (WUI_\w+)\[\s*\]\s*PROGMEM\s*=', src):
        i, ins, start = m.end(), False, m.end()
        while i < len(src):
            ch = src[i]
            if ins:
                if ch == '\\':
                    i += 2; continue
                if ch == '"':
                    ins = False
            else:
                if ch == '"':
                    ins = True
                elif ch == ';':
                    break
            i += 1
        segs[m.group(1)] = literals(src[start:i])
    return segs


def tag_check(page: str):
    """标签闭合自检，返回 (未闭合列表, 嵌套错误列表)。"""
    stack, errs = [], []
    for m in re.finditer(r'<(/?)([a-zA-Z][\w-]*)[^>]*?(/?)>', page):
        closing, tag, selfclose = m.group(1), m.group(2).lower(), m.group(3)
        if tag in VOID or selfclose:
            continue
        if not closing:
            stack.append(tag)
        else:
            if not stack:
                errs.append(f"多余闭合 </{tag}>")
            elif stack[-1] != tag:
                errs.append(f"不匹配：期望 </{stack[-1]}> 实际 </{tag}>")
                if tag in stack:
                    while stack and stack.pop() != tag:
                        pass
            else:
                stack.pop()
    return stack, errs


def main() -> int:
    src = open(HDR, encoding="utf-8").read()
    ota = open(OTA, encoding="utf-8").read()
    S = extract(src)

    reserve_m = re.search(r'#define WUI_RESERVE\s+(\d+)', ota)
    reserve = int(reserve_m.group(1)) if reserve_m else 0

    print("从固件抠出的页面段：")
    for k, v in S.items():
        print(f"  {k:14s} {len(v):6d} 字符")

    # ---- 还原主页（模拟一份配置：节点01 / air / 加密开 / 915.0 / SF7 / 125 / CR5 / 20dBm）----
    status = ('<span>节点 <b>01</b></span><span>角色 <b>air</b></span>'
              '<span class="st"><i></i>加密已启用</span>')
    fields = (
        '<div class="wide"><label>频率配置</label><div class="row">'
        '<select id="fi" name="freq_int">'
        + ''.join(f'<option{" selected" if v == 915 else ""}>{v}</option>' for v in range(862, 931))
        + '</select>'
        + '<select id="fd1" name="freq_d1">'
        + ''.join(f'<option{" selected" if v == 1 else ""}>{v}</option>' for v in range(10))
        + '</select><select id="fd2" name="freq_d2">'
        + ''.join(f'<option{" selected" if v == 2 else ""}>{v}</option>' for v in range(10))
        + '</select><select id="fd3" name="freq_d3">'
        + ''.join(f'<option{" selected" if v == 3 else ""}>{v}</option>' for v in range(10))
        + '</select></div></div>'
        '<div><label for="sf">扩频因子</label><select id="sf" name="sf">'
        + ''.join(f'<option{" selected" if v == 7 else ""}>{v}</option>' for v in (7, 8, 9, 10, 11))
        + '</select></div><div><label for="bw">带宽 kHz</label><select id="bw" name="bw">'
        + ''.join(f'<option{" selected" if v == 125 else ""}>{v}</option>' for v in (125, 250, 500))
        + '</select></div><div><label for="cr">编码率</label><select id="cr" name="cr">'
        + ''.join(f'<option{" selected" if v == 5 else ""}>{v}</option>' for v in (5, 6, 7, 8))
        + '</select></div><div><label for="pw">发射功率 dBm</label><select id="pw" name="power">'
        + ''.join(f'<option{" selected" if v == 20 else ""}>{v}</option>' for v in (10, 14, 17, 20, 22))
        + '</select></div>'
        '<div><label for="nid">节点号 <span id="nidv">1</span></label>'
        '<input id="nid" name="node_id" type="range" min="1" max="254" step="1" value="1"'
        ' oninput="document.getElementById(\'nidv\').textContent=this.value"></div>'
        '<div><label for="rl">角色</label><select id="rl" name="role">'
        '<option value="">未配置</option>'
        '<option value="air" selected>机载 AIR</option>'
        '<option value="gcs">地面站 GCS</option>'
        '<option value="relay">中继 RELAY</option>'
        '</select></div>'
        '<div class="wide"><label for="aws">上电自动进维护模式 <span id="awsv">30</span> 秒（15–60）</label>'
        '<input id="aws" name="auto_wifi_s" type="range" min="15" max="60" step="1" value="30"'
        ' oninput="document.getElementById(\'awsv\').textContent=this.value"></div>')

    main_page = (S['WUI_HEAD'] + S['WUI_SUB_OPEN'] + status + S['WUI_SUB_END']
                 + S['WUI_RADIO_A'] + fields + S['WUI_RADIO_B']
                 + S['WUI_CRYPTO'] + S['WUI_OTA'] + S['WUI_FACTORY'] + S['WUI_TAIL'])

    done_page = (S['WUI_HEAD'] + S['WUI_SUB_OPEN'] + status + S['WUI_SUB_END']
                 + S['WUI_DONE_A'] + '<div class="msg">参数已保存，重启后生效。</div>'
                 + S['WUI_DONE_B'])

    open(OUT_MAIN, "w", encoding="utf-8").write(main_page)
    open(OUT_DONE, "w", encoding="utf-8").write(done_page)

    ok = True
    print(f"\n主页   {len(main_page):6d} 字节 -> {os.path.relpath(OUT_MAIN, ROOT)}")
    print(f"回执页 {len(done_page):6d} 字节 -> {os.path.relpath(OUT_DONE, ROOT)}")
    biggest = max(len(main_page), len(done_page))
    print(f"String 预留 WUI_RESERVE = {reserve}，最大页余量 {reserve - biggest}")
    if biggest > reserve:
        print("  ✗ 页面超过预留容量（String 会再 realloc 一次，建议调大 WUI_RESERVE）")
        ok = False

    for name, page in (("主页", main_page), ("回执页", done_page)):
        unclosed, errs = tag_check(page)
        if unclosed or errs:
            ok = False
            print(f"  ✗ {name} 标签问题：未闭合 {unclosed} 错误 {errs}")
        else:
            print(f"  OK {name} 标签全闭合")

    must = ['id="k"', 'name="key"', 'action="/set"', 'action="/crypto"', 'action="/update"',
            'action="/exit"', 'action="/factory"', 'name="firmware"', 'name="off"',
            '请输入正确的格式', '恢复出厂默认',
            'enctype="multipart/form-data"', 'id="kerr"',
            'name="freq_int"', 'name="freq_d1"', 'name="freq_d2"', 'name="freq_d3"',
            'name="power"', 'name="node_id"',
            'name="role"', 'name="auto_wifi_s"', 'type="range"']
    missing = [f for f in must if f not in main_page]
    if missing:
        ok = False
        print(f"  ✗ 主页缺关键元素: {missing}")
    else:
        print("  OK 主页关键元素齐全（含密钥校验提示 '请输入正确的格式'）")

    print("\n结果:", "全部通过" if ok else "有问题，见上")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
