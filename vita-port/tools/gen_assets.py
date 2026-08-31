#!/usr/bin/env python3
"""Generate PS Vita LiveArea assets (sce_sys) for the J2ME Player VPK.

Run from repo root:  python3 tools/gen_assets.py
Outputs into assets/sce_sys/ following the standard VitaSDK HelloWorld
layout:
    sce_sys/icon0.png                          128x128  bubble icon
    sce_sys/livearea/contents/bg.png           840x500  LiveArea background
    sce_sys/livearea/contents/startup.png      280x158  start button image
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "sce_sys")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

BG_TOP = (24, 32, 64)      # dark navy
BG_BOT = (10, 14, 30)      # near black
ACCENT = (94, 197, 84)     # Java green
ACCENT2 = (80, 160, 240)   # soft blue
WHITE = (240, 244, 248)


def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def centered(d, xy_wh, text, font, fill):
    x, y, w, h = xy_wh
    tw, th = d.textbbox((0, 0), text, font=font)[2:]
    d.text((x + (w - tw) // 2, y + (h - th) // 2), text, font=font, fill=fill)


def icon0():
    """128x128 bubble icon: the classic Java steaming coffee cup."""
    img = vgrad(128, 128, BG_TOP, BG_BOT)
    d = ImageDraw.Draw(img)
    java_orange = (231, 111, 0)    # Java brand orange
    java_orange_dk = (180, 84, 0)
    java_orange_lt = (247, 148, 52)
    java_blue = (83, 130, 161)     # Java brand blue

    # steam: two wavy strands above the cup
    for cx, phase in ((54, 0.0), (74, 2.1)):
        pts = []
        for i in range(25):
            t = i / 24.0
            x = cx + 7 * math.sin(math.pi * 2.2 * t + phase)
            y = 44 - t * 30
            pts.append((x, y))
        d.line(pts, fill=java_orange_lt, width=4, joint="curve")
        d.ellipse([pts[0][0] - 2, pts[0][1] - 2,
                   pts[0][0] + 2, pts[0][1] + 2], fill=java_orange_lt)

    # handle (behind body)
    d.arc([84, 58, 106, 86], start=-70, end=110, fill=java_orange_dk, width=6)

    # cup body + rim
    d.rounded_rectangle([38, 52, 90, 100], radius=10, fill=java_orange)
    d.ellipse([34, 44, 94, 62], fill=java_orange_dk)
    d.ellipse([38, 47, 90, 59], fill=java_orange_lt)
    # body highlight
    d.line([45, 64, 45, 92], fill=java_orange_lt, width=3)

    # saucer
    d.ellipse([28, 96, 100, 108], fill=(70, 78, 92))

    # J2ME tag
    f = ImageFont.truetype(FONT, 15)
    centered(d, (0, 108, 128, 20), "J2ME", f, java_blue)
    img.save(os.path.join(OUT, "icon0.png"))


def bg():
    """840x500 LiveArea background."""
    img = vgrad(840, 500, BG_TOP, BG_BOT)
    d = ImageDraw.Draw(img)
    # faint oversized glyphs
    f_ghost = ImageFont.truetype(FONT, 300)
    d.text((430, 120), "J", font=f_ghost, fill=(255, 255, 255, 8))
    # title block
    f_title = ImageFont.truetype(FONT, 72)
    f_sub = ImageFont.truetype(FONT, 30)
    d.text((60, 150), "J2ME", font=f_title, fill=ACCENT)
    d.text((64, 240), "Player", font=f_title, fill=WHITE)
    d.text((66, 330), "phoneME MIDP 2.0 on PS Vita", font=f_sub, fill=ACCENT2)
    # bottom accent bar
    d.rectangle([0, 484, 840, 500], fill=ACCENT)
    img.save(os.path.join(OUT, "livearea", "contents", "bg.png"))


def startup():
    """280x158 start button image."""
    img = vgrad(280, 158, BG_TOP, BG_BOT)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, 272, 150], radius=10, outline=ACCENT, width=2)
    f = ImageFont.truetype(FONT, 44)
    centered(d, (8, 8, 264, 100), "START", f, WHITE)
    f2 = ImageFont.truetype(FONT, 18)
    centered(d, (8, 104, 264, 40), "J2ME PLAYER", f2, ACCENT2)
    img.save(os.path.join(OUT, "livearea", "contents", "startup.png"))


def main():
    os.makedirs(os.path.join(OUT, "livearea", "contents"), exist_ok=True)
    icon0()
    bg()
    startup()
    print("assets written to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
