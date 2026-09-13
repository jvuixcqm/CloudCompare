#!/usr/bin/env python3
"""生成 TiffViewer.iconset（Apple 图标资源目录）。

跟 ../gen_icon.ps1 用的是同一套热力图 LUT + 等距阶梯曲面渲染算法，
只是把目标格式换成 macOS 的 .iconset（一堆按规定命名的 PNG），而不是 Windows .ico。

用法：
    python3 generate_icon.py

产物：本目录下的 TiffViewer.iconset/ 文件夹。
最后一步（把 iconset 编译成 .icns）必须在真正的 macOS 上跑，本机（Windows）没有 iconutil：
    iconutil -c icns TiffViewer.iconset -o TiffViewer.icns
"""
import math
import os

from PIL import Image, ImageDraw

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ICONSET_DIR = os.path.join(SCRIPT_DIR, "TiffViewer.iconset")

# (文件名, 像素边长) —— Apple iconutil 要求的标准 iconset 命名
ICONSET_SIZES = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

SUPERSAMPLE = 4  # 先按 4x 分辨率画多边形再降采样，模拟 gfx.SmoothingMode = AntiAlias


def lut_color(t: float):
    """6 段 LUT：Blue->Cyan->Green->Yellow->Red->Magenta->White，跟 TiffBmpLoader.cpp 的 lutEntry() 一致。"""
    i = int(max(0.0, min(1.0, t)) * 1536)
    if i < 256:
        r, g, b = 0, i, 255
    elif i < 512:
        r, g, b = 0, 255, 511 - i
    elif i < 768:
        r, g, b = i - 512, 255, 0
    elif i < 1024:
        r, g, b = 255, 1023 - i, 0
    elif i < 1280:
        r, g, b = 255, 0, i - 1024
    else:
        r, g, b = 255, min(i - 1280, 255), 255
    return (r, g, b)


def brighten(c, f):
    return tuple(min(255, int(v * f)) for v in c)


def make_bitmap(size: int) -> Image.Image:
    ss = size * SUPERSAMPLE
    cols = 8 if size >= 128 else (6 if size >= 48 else 5)
    rows = cols

    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    margin = ss * 0.06
    hw = (ss - 2.0 * margin) / (cols + rows + 1.0)
    hh = hw * 0.48
    scx = ss * 0.50
    zsc = hw * 1.8
    scy = ss * 0.50 + (cols * zsc * 0.25)

    for row in range(rows - 1, -1, -1):
        for col in range(cols):
            t = (col + (rows - 1 - row)) / float(cols + rows - 2)
            ht = t * t * t

            u = col - (cols - 1) * 0.5
            v = row - (rows - 1) * 0.5
            cx = scx + (u - v) * hw
            cy = scy + (u + v) * hh - ht * zsc * cols

            top = [
                (cx, cy - hh),
                (cx + hw, cy),
                (cx, cy + hh),
                (cx - hw, cy),
            ]

            if size >= 32 and ht > 0.01:
                sh = ht * zsc * cols * 0.32
                left_poly = [
                    (cx - hw, cy),
                    (cx, cy + hh),
                    (cx, cy + hh + sh),
                    (cx - hw, cy + sh),
                ]
                right_poly = [
                    (cx + hw, cy),
                    (cx, cy + hh),
                    (cx, cy + hh + sh),
                    (cx + hw, cy + sh),
                ]
                bc = lut_color(t)
                draw.polygon(left_poly, fill=brighten(bc, 0.55) + (255,))
                draw.polygon(right_poly, fill=brighten(bc, 0.72) + (255,))

            tc = lut_color(t)
            draw.polygon(top, fill=tc + (255,))

            if size >= 24:
                pa = 50 if size >= 64 else 35
                pw = max(1, round(ss * 0.004))
                outline = (0, 0, 0, pa)
                closed = top + [top[0]]
                draw.line(closed, fill=outline, width=pw, joint="curve")

    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(ICONSET_DIR, exist_ok=True)
    cache: dict[int, Image.Image] = {}
    for filename, size in ICONSET_SIZES:
        if size not in cache:
            cache[size] = make_bitmap(size)
        out_path = os.path.join(ICONSET_DIR, filename)
        cache[size].save(out_path)
        print(f"  {filename}  ({size}x{size})")

    print(f"\nDone: {ICONSET_DIR}")
    print("在真正的 macOS 上运行以下命令编译出 .icns：")
    print(f'  cd "{SCRIPT_DIR}" && iconutil -c icns TiffViewer.iconset -o TiffViewer.icns')


if __name__ == "__main__":
    main()
