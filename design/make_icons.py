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

from PIL import Image, ImageDraw

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
    d = ImageDraw.Draw(img)

    # 频谱波形：粗线，占上方 2/3
    pad = 40
    w = BASE - 2 * pad
    pts = [0.26, 0.58, 0.32, 0.74, 0.46, 0.90, 0.36, 0.62,
           0.54, 0.98, 0.40, 0.68, 0.50, 0.80, 0.30, 0.54]
    xs = [pad + int(w * i / (len(pts) - 1)) for i in range(len(pts))]
    ys = [54 + int((1 - v) * 100) for v in pts]
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
