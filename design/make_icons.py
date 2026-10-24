#!/usr/bin/env python3
"""EM Studio / EM-SDR 图标定稿。

第一版问题：1px 线在 48px 下消失、留白过多、没有主体。
A/B/C 三组候选后确定的解法：

    EM Studio —— 一个方框（设备）+ 框内三条参数条 + 一个实心滑块
                 （之前是散落的横线，读不出"设备"这个主体）
    EM-SDR    —— 粗白波形（频谱）+ 下方亮度递减的方块矩阵（瀑布）
                 （波形给辨识度，矩阵给"信号在滚动"的感觉）

共用：深色卡 #0d0d0d、发丝边 #2c2c2c、零圆角、无渐变无阴影、
右上角唯一彩色元素 #34d399。线条至少 6px（@256），缩到 48 仍有 1px。
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw

BG = (13, 13, 13)
EDGE = (44, 44, 44)
FG = (237, 237, 237)
OK = (52, 211, 153)
G1 = (150, 150, 150)
G2 = (96, 96, 96)
G3 = (52, 52, 52)

SIZES = (512, 256, 128, 64, 48)
OUT = Path("/home/sun/EMlink/design/icons")
BASE = 256          # 所有坐标按 256 设计，再等比缩放


def _tri_mesh(size: int, step: int = 30) -> Image.Image:
    """EMlink logo 同款三角网格：水平线 + 两组 60 度斜线。

    返回 L 模式遮罩（线条处 255，空白 0），由调用方再乘透明度与径向衰减。
    """
    layer = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(layer)
    h = step * 0.8660          # 等边三角形的高

    y = 0.0
    while y <= size:
        d.line([(0, y), (size, y)], fill=255, width=1)
        y += h

    # 两组 60 度斜线，斜率 tan(60°) = 1.732
    slope = 1.7320
    span = int(slope * size) + size
    x = -span
    while x <= size + span:
        d.line([(x, 0), (x + size * slope, size)], fill=255, width=1)
        d.line([(x, size), (x + size * slope, 0)], fill=255, width=1)
        x += step

    return layer


def _radial_fade(size: int, inner: float = 0.42,
                 outer: float = 1.02) -> Image.Image:
    """径向遮罩：中心 0（完全消失）→ 边缘 255（可见）。

    inner 以内全 0，outer 以外全 255，中间线性过渡。
    """
    arr = np.zeros((size, size), dtype=np.float32)
    c = (size - 1) / 2.0
    ys, xs = np.mgrid[0:size, 0:size]
    dist = np.sqrt((xs - c) ** 2 + (ys - c) ** 2) / c     # 0 = 中心，1 = 边中点
    t = (dist - inner) / max(outer - inner, 1e-6)
    arr = np.clip(t, 0.0, 1.0)
    return Image.fromarray((arr * 255).astype(np.uint8), mode="L")


def _add_mesh(img: Image.Image, strength: int = 34) -> None:
    """在图标卡上叠加淡淡三角网格，中心淡出。

    strength 是网格线在边缘处的最大不透明度（0-255）。34 压在 #0d0d0d 上
    约等于 #262626，暗到不抢主体、亮到边缘能看出是网。
    """
    size = img.size[0]
    mesh = _tri_mesh(size)
    fade = _radial_fade(size)
    alpha = ImageChops.multiply(mesh, fade)
    alpha = alpha.point(lambda v: int(v * strength / 255))

    layer = Image.new("RGBA", (size, size), (128, 128, 128, 0))
    layer.putalpha(alpha)
    img.alpha_composite(layer)


def _blank() -> Image.Image:
    return Image.new("RGBA", (BASE, BASE), (0, 0, 0, 0))


def _card(img: Image.Image) -> None:
    d = ImageDraw.Draw(img)
    d.rectangle([2, 2, BASE - 3, BASE - 3], fill=BG, outline=EDGE, width=2)


def _dot(img: Image.Image) -> None:
    d = ImageDraw.Draw(img)
    cx, cy, r = BASE - 32, 32, 7
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=OK)


def studio() -> Image.Image:
    """设备方框 + 框内参数条 + 实心滑块。"""
    img = _blank()
    _card(img)
    _add_mesh(img)          # 先铺背景网格，主体压在上面
    d = ImageDraw.Draw(img)

    # 设备外框：粗，作为主体轮廓
    bx0, by0, bx1, by1 = 44, 62, BASE - 44, 200
    d.rectangle([bx0, by0, bx1, by1], outline=FG, width=6)

    # 框内三条参数条
    ix = bx0 + 18
    iw = (bx1 - bx0) - 36
    for i, y in enumerate((by0 + 30, by0 + 62, by0 + 94)):
        # 第一行填满亮线（已配置），后两行短灰线
        if i == 0:
            d.rectangle([ix, y - 4, ix + iw, y + 4], fill=FG)
        else:
            d.rectangle([ix, y - 4, ix + int(iw * 0.62), y + 4], fill=G2)

    # 第三条上压一个实心滑块，表示正在调
    sx = ix + int(iw * 0.72)
    sy = by0 + 94
    h = 15
    d.rectangle([sx - h, sy - h, sx + h, sy + h], fill=FG)

    _dot(img)
    return img


def sdr() -> Image.Image:
    """粗波形 + 亮度递减的瀑布方块矩阵。"""
    img = _blank()
    _card(img)
    _add_mesh(img)          # 先铺背景网格，主体压在上面
    d = ImageDraw.Draw(img)

    # 频谱波形：13 个点、关于中心严格镜像。
    # 关键：谷底固定在同一水平线上（base 常数），只有峰高按包络递减，
    # 主峰独占 1.0、相邻次峰明显压低，突出"收到一个强信号"这件事。
    pad = 40
    w = BASE - 2 * pad
    n = 13
    center = n // 2
    top_y, bot_y = 44, 136

    base = 0.12          # 谷底高度，全图统一 → 水平基线
    peak_gain = 0.72     # 次峰最大幅度（主峰为 1.0，不乘这个系数）
    falloff = 2.1        # 包络衰减指数，越大主峰越突出

    ys = []
    for i in range(n):
        dist = abs(i - center) / center
        if i == center:
            h = 1.0                                  # 主峰
        elif (i - center) % 2 == 0:
            h = base + peak_gain * (1.0 - dist) ** falloff  # 次峰，递减
        else:
            h = base                                 # 谷，严格水平
        ys.append(bot_y - int((bot_y - top_y) * h))
    xs = [pad + int(w * i / (n - 1)) for i in range(n)]
    d.line(list(zip(xs, ys)), fill=FG, width=7, joint="curve")

    # 瀑布矩阵：3 行 5 列，宽度与波形严格对齐，亮度自上而下衰减
    cell, gap = 26, 37
    x0, y0 = pad, 152
    rows = [(1, 4), (0, 3), (2, 4)]
    shades = (FG, G1, G2)
    for r, cols in enumerate(rows):
        for c in range(5):
            x = x0 + c * gap
            y = y0 + r * 32
            if c in cols:
                d.rectangle([x, y, x + cell, y + cell], fill=shades[r])
            else:
                d.rectangle([x, y, x + cell, y + cell], outline=(30, 30, 30), width=2)

    _dot(img)
    return img


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in (("em-studio", studio), ("em-sdr", sdr)):
        base = fn()
        for size in SIZES:
            img = base.resize((size, size), Image.LANCZOS)
            path = OUT / f"{name}-{size}.png"
            img.save(path, "PNG")
        base.resize((512, 512), Image.LANCZOS).save(OUT / f"{name}.png", "PNG")
        print(f"  {name}: {len(SIZES)} 个尺寸")

    # 对比预览
    a, b = studio(), sdr()
    big = 2
    c = Image.new("RGB", (BASE * 2 * big + 90, BASE * big + 40), (24, 24, 24))
    c.paste(a.resize((BASE * big, BASE * big), Image.LANCZOS), (30, 20), a.resize((BASE * big, BASE * big), Image.LANCZOS))
    c.paste(b.resize((BASE * big, BASE * big), Image.LANCZOS), (BASE * big + 60, 20), b.resize((BASE * big, BASE * big), Image.LANCZOS))
    c.save("/home/sun/emsdr-logs/final_icons.png")
    print(f"  预览: /home/sun/emsdr-logs/final_icons.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
