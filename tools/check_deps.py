#!/usr/bin/env python3
"""EMlink 三层（应用-中间-驱动）依赖体检。
只做静态读文件，不执行任何代码，不阻塞任何人：
  默认模式  → 只报告，退出码 0
  --strict  → 有违规时退出码 1（留给未来 CI 用，现阶段没人调用）
分层定义（本文件是唯一事实来源）：
  应用层   firmware/src/app/
  中间层   firmware/src/link/  firmware/src/net/  firmware/src/service/
  驱动层   firmware/src/drivers/  firmware/src/boards/  firmware/src/radios/
  基础支撑 firmware/src/rtos.{h,cpp}、service/watchdog.*、service/nvs_cfg.*（横切，各层可用）
  入口     firmware/src/main.cpp（例外，允许 include 全部）
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "firmware", "src")

MIDDLE_PREFIX = ("firmware/src/link/", "firmware/src/net/", "firmware/src/service/")
DRIVER_PREFIX = ("firmware/src/drivers/", "firmware/src/boards/", "firmware/src/radios/")

# 检查 2 的目标动作：只允许出现在白名单文件里（名单带理由，见 WHITELIST）
ACTIONS = ("frame_encode", "frame_decode", "crypto_seal_frame", "crypto_open_frame",
           "rtos_tx_enqueue", "radio_transmit", "radio_transmit_prio", "radio_startReceive")
ACTION_RE = re.compile(r"\b(" + "|".join(ACTIONS) + r")\s*\(")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

# 检查 2 白名单：rel_path -> (理由, 是否待收敛)
WHITELIST = {
    "firmware/src/link/frame_codec.cpp": ("实现本体", False),
    "firmware/src/service/crypto.cpp": ("实现本体", False),
    "firmware/src/drivers/radio.cpp": ("实现本体", False),
    "firmware/src/link/mac.cpp": ("唯一允许碰射频的地方（P0-2 射频单所有者）", False),
    "firmware/src/net/mesh.cpp": ("中间层出网口", False),
    "firmware/src/service/mav_router.cpp": ("待收敛：封装动作最终要收进中间层出口", True),
}


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def is_base_support(r):
    return r in ("firmware/src/rtos.h", "firmware/src/rtos.cpp") or \
        re.match(r"firmware/src/service/(watchdog|nvs_cfg|role_state)\.(h|cpp)$", r) is not None


def layer_of(r):
    if r == "firmware/src/main.cpp":
        return "entry"
    if r.startswith("firmware/src/app/"):
        return "app"
    if r.startswith(MIDDLE_PREFIX):
        return "middle"
    if r.startswith(DRIVER_PREFIX):
        return "driver"
    return "other"


def iter_sources():
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ("build", "lib", ".git")]
        for name in filenames:
            if name.endswith((".cpp", ".h")):
                yield os.path.join(dirpath, name)


def scan_include_violations():
    out = []
    for path in iter_sources():
        r = rel(path)
        layer = layer_of(r)
        if layer not in ("middle", "driver"):
            continue  # 应用层不受限；基础支撑/入口豁免
        base = os.path.dirname(r)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for ln, line in enumerate(f, 1):
                m = INCLUDE_RE.match(line)
                if not m:
                    continue
                target = os.path.normpath(os.path.join(base, m.group(1))).replace(os.sep, "/")
                if not target.startswith("firmware/src/"):
                    continue
                tl = layer_of(target)
                if layer == "middle" and tl == "app":
                    out.append((r, ln, "中间层 include 了 app/", m.group(1)))
                elif layer == "driver" and tl == "middle" and not is_base_support(target):
                    out.append((r, ln, "驱动层 include 了中间层", m.group(1)))
    return out


def scan_action_hits():
    hard, soft, conv = [], [], []
    for path in iter_sources():
        r = rel(path)
        if not r.endswith(".cpp"):
            continue  # 头文件只有声明/inline，动作体检只看实现体
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for ln, line in enumerate(f, 1):
                m = ACTION_RE.search(line)
                if not m:
                    continue
                rec = (r, ln, m.group(1))
                if r in WHITELIST:
                    (conv if WHITELIST[r][1] else soft).append(rec)
                else:
                    hard.append(rec)
    return hard, soft, conv


def main():
    violations = scan_include_violations()
    hard, soft, conv = scan_action_hits()

    p = print
    p("=== EMlink 分层体检（应用-中间-驱动）===")
    p("[硬规则] 反向依赖")
    if violations:
        for r, ln, why, inc in violations:
            p("  ✗ firmware/src/%s:%d  → %s（%s）" % (r[len("firmware/src/"):], ln, why, inc))
    else:
        p("  ✓ 无")

    p("[越层动作] 中间件内部动作出现在非白名单文件")
    if hard:
        for r, ln, fn in hard:
            p("  ✗ %s:%d  %s(" % (r, ln, fn))
    else:
        p("  ✗ 无")

    p("[越层动作] 白名单内 · 待收敛（改造目标，见 ROADMAP）")
    if conv:
        for r, ln, fn in conv:
            p("  · %s:%d  %s(" % (r, ln, fn))
    else:
        p("  · 无（收敛完成，可删除该白名单项）")

    p("---")
    p("统计：硬规则违规 %d 处 · 越层动作违规 %d 处 · 待收敛命中 %d 处"
      % (len(violations), len(hard), len(conv)))
    if not violations and not hard and not conv:
        p("✅ 分层干净")

    if "--strict" in sys.argv and (violations or hard):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
