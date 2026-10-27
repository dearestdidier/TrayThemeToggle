# -*- coding: utf-8 -*-
"""生成托盘用的太阳 / 月亮 / 应用图标（多尺寸 ICO）。"""
import math
import os
from PIL import Image, ImageDraw

S = 1024
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "res")
ICO_SIZES = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]


def vgrad_circle(size, top, bottom):
    """竖直渐变的实心圆，返回 RGBA 图。"""
    grad = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(grad)
    for y in range(size):
        t = y / max(1, size - 1)
        col = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        d.line([(0, y), (size, y)], fill=col)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def make_sun():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    R = int(S * 0.255)
    disc = vgrad_circle(2 * R, (255, 212, 96), (255, 146, 20))
    img.alpha_composite(disc, (S // 2 - R, S // 2 - R))

    d = ImageDraw.Draw(img)
    ray_len = int(S * 0.125)
    ray_w = int(S * 0.072)
    inner = R + int(S * 0.062)
    cx = cy = S // 2
    ray_col = (255, 170, 42, 255)
    for i in range(8):
        a = math.radians(i * 45)
        ca, sa = math.cos(a), math.sin(a)
        x0, y0 = cx + ca * inner, cy + sa * inner
        x1, y1 = cx + ca * (inner + ray_len), cy + sa * (inner + ray_len)
        d.line([(x0, y0), (x1, y1)], fill=ray_col, width=ray_w)
        for (px, py) in ((x0, y0), (x1, y1)):
            r = ray_w / 2.0
            d.ellipse([px - r, py - r, px + r, py + r], fill=ray_col)
    return img


def make_moon():
    R = int(S * 0.335)
    body = vgrad_circle(2 * R, (208, 228, 255), (92, 138, 224))
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    canvas.alpha_composite(body, (S // 2 - R, S // 2 - R))

    hole = Image.new("L", (S, S), 0)
    hr = int(R * 0.94)
    ox = S // 2 + int(R * 0.56)
    oy = S // 2 - int(R * 0.44)
    ImageDraw.Draw(hole).ellipse([ox - hr, oy - hr, ox + hr, oy + hr], fill=255)

    alpha = canvas.split()[3]
    alpha = Image.composite(Image.new("L", (S, S), 0), alpha, hole)
    canvas.putalpha(alpha)
    return canvas


def make_app():
    """圆角方块：上半琥珀（浅色），下半深蓝（深色）。"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pad = int(S * 0.06)
    box = [pad, pad, S - pad, S - pad]
    radius = int(S * 0.22)

    # 上半浅色
    top = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(top).rectangle([0, 0, S, S // 2], fill=(255, 190, 60, 255))
    # 下半深色
    bot = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(bot).rectangle([0, S // 2, S, S], fill=(32, 44, 74, 255))

    # 渐变叠加让层次更好
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(g)
    for y in range(S):
        t = y / (S - 1)
        if y < S // 2:
            c = (255, 205, 100, 255)
        else:
            c = (26, 36, 62, 255)
        gd.line([(0, y), (S, y)], fill=c)

    merged = Image.alpha_composite(top, bot)
    merged = Image.alpha_composite(merged, Image.blend(Image.new("RGBA", (S, S), (0, 0, 0, 0)), g, 0.0))

    # 圆角遮罩
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(merged, (0, 0), mask)

    # 中缝细线
    d = ImageDraw.Draw(out)
    d.line([(pad + 6, S // 2), (S - pad - 6, S // 2)], fill=(255, 255, 255, 90), width=4)

    # 上半画个小太阳，下半画个月牙，16px 下也能看出含义
    scx, scy, sr = S // 2, int(S * 0.27), int(S * 0.105)
    d.ellipse([scx - sr, scy - sr, scx + sr, scy + sr], fill=(255, 255, 255, 235))
    for i in range(8):
        a = math.radians(i * 45)
        r0, r1 = sr * 1.42, sr * 1.95
        d.line([(scx + math.cos(a) * r0, scy + math.sin(a) * r0),
                (scx + math.cos(a) * r1, scy + math.sin(a) * r1)],
               fill=(255, 255, 255, 235), width=max(4, int(S * 0.014)))

    mcx, mcy, mr = S // 2, int(S * 0.735), int(S * 0.115)
    d.ellipse([mcx - mr, mcy - mr, mcx + mr, mcy + mr], fill=(255, 255, 255, 235))
    d.ellipse([mcx - int(mr * 0.35), mcy - int(mr * 1.28),
               mcx + int(mr * 1.28), mcy + int(mr * 0.35)], fill=(26, 36, 62, 255))
    return out


def save_ico(img, path):
    img.save(path, format="ICO", sizes=ICO_SIZES)
    print("wrote", path)


def main():
    os.makedirs(OUT, exist_ok=True)
    save_ico(make_sun(), os.path.join(OUT, "sun.ico"))
    save_ico(make_moon(), os.path.join(OUT, "moon.ico"))
    save_ico(make_app(), os.path.join(OUT, "app.ico"))


if __name__ == "__main__":
    main()
