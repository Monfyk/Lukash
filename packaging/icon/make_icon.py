#!/usr/bin/env python3
"""Малює іконку застосунку (гриф з трьома нотами) у PNG різних розмірів та ICO."""
import os
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__))
def draw(size):
    S = 1024; im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle((40, 40, S-40, S-40), radius=210, fill=(26, 29, 25, 255))
    for i in range(5):  # струни
        x = 230 + i*141
        d.line((x, 200, x, S-200), fill=(236, 239, 234, 255), width=26)
    for j in range(3):  # лади
        y = 360 + j*150
        d.line((180, y, S-180, y), fill=(120, 128, 116, 255), width=18)
    teal = (60, 195, 222, 255)
    for (i, j) in [(1, 0), (2, 1), (3, 0)]:
        cx, cy = 230 + i*141, 285 + j*150 + 75
        d.ellipse((cx-78, cy-78, cx+78, cy+78), fill=teal)
    return im.resize((size, size), Image.LANCZOS)
if __name__ == "__main__":
    big = draw(512); big.save(os.path.join(HERE, "icon-512.png"))
    draw(256).save(os.path.join(HERE, "icon-256.png"))
    big.save(os.path.join(HERE, "icon.ico"), sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
    for name, px in {"mdpi":48,"hdpi":72,"xhdpi":96,"xxhdpi":144,"xxxhdpi":192}.items():
        p = os.path.join(HERE, "..", "android", "res", f"mipmap-{name}"); os.makedirs(p, exist_ok=True)
        draw(px).save(os.path.join(p, "ic_launcher.png"))
    print("ok")
