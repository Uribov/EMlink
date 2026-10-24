#!/usr/bin/env python3
"""生成 EM Studio / EM-SDR 的桌面图标。

风格必须与两个 GUI 一致：
    底色 #0d0d0d   线 #2c2c2c / #232323   前景 #ededed
    状态点 #34d399   零圆角   1px 发丝线   无渐变无阴影

两个图标共用一套骨架（深色方卡 + 发丝边框 + 右上角状态点），
图形部分各表其职：
    EM Studio  —— 参数行 + 选中滑块（它是个配置工具）
    EM-SDR     —— 频谱折线 + 瀑布灰条（它是个信号工具）

输出：512 / 256 / 128 / 64 / 48 五种 PNG，外加一份 SVG 源。
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

BG = (13, 13, 13)
LINE = (44, 44, 44)
LINE2 = (35, 35, 35)
FG = (237, 237, 237)
DIM = (74, 74, 74)
OK = (52, 211, 153)

SIZES = (512, 256, 128, 64, 48)
OUT = Path("/home/sun/EMlink/design/icons")


def _card(size: int) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    """深色方卡 + 1px 发丝边框。零圆角。"""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    inset = max(1, size // 64)
    d.rectangle([inset, inset, size - 1 - inset, size - 1 - inset],
                fill=BG, outline=LINE, width=max(1, size // 256))
    return img, d


def _dot(d: ImageDraw.ImageDraw, size: int) -> None:
    """右上角状态点：GUI 里唯一的彩色元素。"""
    r = max(2, size // 40)
    cx, cy = size - size // 7, size // 7
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=OK)


def draw_studio(size: int) -> Image.Image:
    img, d = _card(size)

    # 可用区域：四周留 1/9，比 SDR 那张略宽，让三条行线摊开
    pad = size // 9
    w = size - 2 * pad

    # 三条参数行，间距拉开填满竖向空间
    rows = 3
    gap = (size - 2 * pad) // (rows + 1)
    lw = max(1, size // 150)

    for i in range(rows):
        y = pad + gap * (i + 1)
        d.line([pad, y, pad + w, y], fill=LINE2, width=lw)

    # 第二条上放一个实心滑块（表示"正在改这一项"）
    y2 = pad + gap * 2
    bx = pad + int(w * 0.60)
    half = max(3, size // 18)
    d.rectangle([bx - half, y2 - half, bx + half, y2 + half], fill=FG)

    # 第一行左端一段亮线，像已填的数值
    y1 = pad + gap
    d.line([pad, y1, pad + int(w * 0.40), y1], fill=FG, width=lw)

    # 第三行右端一段暗线，暗示未填
    y3 = pad + gap * 3
    d.line([pad + int(w * 0.70), y3, pad + w, y3], fill=DIM, width=lw)

    _dot(d, size)
    return img


def draw_sdr(size: int) -> Image.Image:
    img, d = _card(size)

    pad = size // 7
    top = size // 3
    bottom = size - pad - size // 8
    w = size - 2 * pad

    # 频谱折线：一组固定形状的折点，看起来像噪声起伏
    pts = [0.30, 0.52, 0.34, 0.66, 0.44, 0.78, 0.36, 0.60,
           0.50, 0.88, 0.40, 0.62, 0.46, 0.70, 0.32, 0.50]
    xs = [pad + int(w * i / (len(pts) - 1)) for i in range(len(pts))]
    ys = [top + int((1 - v) * (bottom - top) * 0.92) for v in pts]
    d.line(list(zip(xs, ys)), fill=FG, width=max(1, size // 140),
           joint="curve")

    # 折线下方淡填充（画成几条渐矮的竖线，避免 alpha 合成开销）
    base = bottom
    for x, y in zip(xs, ys):
        d.line([x, y, x, base], fill=(40, 40, 40), width=max(1, w // 60))

    # 瀑布：三条灰阶横条，越往下越暗
    wf_top = bottom + size // 16
    bar_h = max(1, size // 46)
    shades = [(150, 150, 150), (96, 96, 96), (56, 56, 56), (34, 34, 34)]
    for i, col in enumerate(shades):
        y = wf_top + i * (bar_h + max(1, size // 90))
        if y > size - pad // 2:
            break
        # 每条长度不同，做出"信号扫过"的不均匀感
        frac = (0.92, 0.66, 0.80, 0.44)[i]
        d.rectangle([pad, y, pad + int(w * frac), y + bar_h], fill=col)

    _dot(d, size)
    return img


SVG_TMPL = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">
  <rect x="8" y="8" width="496" height="496" fill="#0d0d0d" stroke="#2c2c2c" stroke-width="2"/>
{body}
  <circle cx="{dx}" cy="{dy}" r="13" fill="#34d399"/>
</svg>
"""

SVG_STUDIO = """  <line x1="85" y1="170" x2="300" y2="170" stroke="#ededed" stroke-width="3"/>
  <line x1="85" y1="256" x2="427" y2="256" stroke="#232323" stroke-width="3"/>
  <line x1="85" y1="342" x2="427" y2="342" stroke="#232323" stroke-width="3"/>
  <rect x="297" y="234" width="46" height="46" fill="#ededed"/>"""

SVG_SDR = """  <polyline points="73,240 98,286 124,252 149,308 175,272 200,340 226,264 251,300 277,232 302,330 328,262 353,296 379,254 404,318 430,268 439,300"
            fill="none" stroke="#ededed" stroke-width="4"/>
  <rect x="73" y="392" width="340" height="12" fill="#969696"/>
  <rect x="73" y="414" width="244" height="12" fill="#606060"/>
  <rect x="73" y="436" width="296" height="12" fill="#383838"/>"""


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)

    jobs = [("em-studio", draw_studio, SVG_STUDIO),
            ("em-sdr", draw_sdr, SVG_SDR)]

    for name, fn, svg_body in jobs:
        for size in SIZES:
            img = fn(size)
            path = OUT / f"{name}-{size}.png"
            img.save(path, "PNG")
            print(f"  {path.name}  {path.stat().st_size} B")

        # 512 的那张另存一份通用名，方便直接引用
        fn(512).save(OUT / f"{name}.png", "PNG")

        dx, dy = 512 - 512 // 7, 512 // 7
        svg = SVG_TMPL.format(body=svg_body, dx=dx, dy=dy)
        (OUT / f"{name}.svg").write_text(svg, encoding="utf-8")
        print(f"  {name}.svg")

    print(f"\n全部输出到 {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
