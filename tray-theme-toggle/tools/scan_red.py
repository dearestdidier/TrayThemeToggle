"""扫描截图里的红色像素，定位"红色正方形"到底在哪。"""
import sys
from PIL import Image


def scan(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    print(f"\n=== {path}  ({w}x{h}) ===")

    reds = []
    for y in range(h):
        for x in range(w):
            r, g, b = im.getpixel((x, y))
            # 明显偏红：红通道高、绿蓝都低
            if r > 120 and r - g > 50 and r - b > 50:
                reds.append((x, y, (r, g, b)))

    if not reds:
        print("  no strong red pixels")
        return

    print(f"  red pixels: {len(reds)} ({100.0*len(reds)/(w*h):.2f}%)")
    xs = [p[0] for p in reds]
    ys = [p[1] for p in reds]
    print(f"  bounding box: x {min(xs)}..{max(xs)}   y {min(ys)}..{max(ys)}")
    print(f"  box size: {max(xs)-min(xs)+1} x {max(ys)-min(ys)+1}")

    # 采样几个点
    step = max(1, len(reds) // 6)
    print("  samples:")
    for p in reds[::step][:6]:
        print(f"    ({p[0]},{p[1]}) rgb={p[2]}")

    # 判断是不是靠近标题栏（顶部 40px）
    top_reds = [p for p in reds if p[1] < 40]
    print(f"  in title bar area (y<40): {len(top_reds)}")
    if top_reds:
        txs = [p[0] for p in top_reds]
        print(f"    title-bar red x range: {min(txs)}..{max(txs)}")
        print(f"    image width {w}, so right edge region starts at {w-60}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        scan(p)
