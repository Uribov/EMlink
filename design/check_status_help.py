#!/usr/bin/env python3
"""核对 status-help.js 的词典覆盖了固件 status 输出的所有指标名。

方法：从 firmware/src/app/cli.cpp 的 status 打印里抽出所有 `xxx=` 的 xxx，
再读 status-help.js 的 ITEMS 键，报告缺了哪些。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path("/home/sun/EMlink")
CLI = REPO / "firmware/src/app/cli.cpp"
HELP = REPO / "tools/em-studio/ui/status-help.js"

src = CLI.read_text(encoding="utf-8", errors="replace")
js = HELP.read_text(encoding="utf-8", errors="replace")

# 只取 status 打印区域：定位含「射频: 就绪」的 printf 块起点，
# 一直到「系统:」那一行结束
start = src.find("射频: 就绪")
end = src.find("射频重建", start)
if start < 0 or end < 0:
    print("找不到 status 打印区域", file=sys.stderr)
    raise SystemExit(1)
block = src[start:end + 200]

# 先去掉 printf 的格式符（%u / %s / %d / %lu …），否则 "u" "s" 会被当成指标名
block = re.sub(r"%[-0-9.]*[a-zA-Z]+", " ", block)

# 抽取 `xxx=` 的 xxx（中文/ASCII 混合，不含引号空格）
# 这几个是正则误判，不是真的指标名：
#   "在线  请求超时" / "否  模块" —— 是 `模块=在线  请求超时=` 这种字面值拼接
#   "err"                        —— 是 err=%d 里的格式化占位
#   "淘汰"                       —— 是 "路由过期=0/淘汰=0" 被 "/" 切开的一半
SKIP = {"在线  请求超时", "否  模块", "err", "淘汰"}

labels = []
for m in re.finditer(r"([\u4e00-\u9fffA-Za-z_][\u4e00-\u9fffA-Za-z_0-9 ]*?)=", block):
    name = m.group(1).strip()
    if name and name not in labels and name not in SKIP:
        labels.append(name)

# 读 JS 词典的键
js_items = js[js.find("const ITEMS = {"):js.find("const CFG = {")]
keys = set(re.findall(r"^\s*'([^']+)':", js_items, re.M))

print(f"固件 status 里共 {len(labels)} 个指标名\n")
missing = []
for name in labels:
    ok = name in keys
    if not ok:
        missing.append(name)
    print(f"  {'O' if ok else 'X'}  {name}")

print()
if missing:
    print(f"缺 {len(missing)} 个解释：")
    for m in missing:
        print(f"  - {m}")
    raise SystemExit(1)
print("全部覆盖")
