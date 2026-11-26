"""Generates VitaSurf's LiveArea images (indexed PNGs, as the Vita requires)."""
import math, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "sce_sys"
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BG_TOP, BG_BOT = (16, 18, 30), (8, 9, 16)
ACCENT, ACCENT2 = (90, 140, 255), (70, 210, 200)

def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img

def glow(img, cx, cy, r, color, strength=110):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).ellipse((cx - r, cy - r, cx + r, cy + r), fill=color + (strength,))
    layer = layer.filter(ImageFilter.GaussianBlur(r * 0.6))
    return Image.alpha_composite(img.convert("RGBA"), layer)

def globe(size, ss=4):
    """Stylised globe: circle with meridians and parallels, drawn supersampled."""
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    lw = max(2, int(S * 0.045))
    m = lw
    d.ellipse((m, m, S - m, S - m), outline=ACCENT + (255,), width=lw)
    cx = S / 2
    for k in (0.42, 0.0):
        w = (S / 2 - m) * k
        if w > 1:
            d.ellipse((cx - w, m, cx + w, S - m), outline=ACCENT2 + (255,), width=lw)
    d.line((cx, m, cx, S - m), fill=ACCENT2 + (255,), width=lw)
    for f in (-0.5, 0, 0.5):
        y = cx + f * (S / 2 - m)
        half = math.sqrt(max(0, (S / 2 - m) ** 2 - (y - cx) ** 2)) - lw
        d.line((cx - half, y, cx + half, y), fill=ACCENT2 + (255,), width=lw)
    return im.resize((size, size), Image.LANCZOS)

def wave(d, w, y, amp, color, width):
    pts = [(x, y + amp * math.sin(x / w * math.pi * 3)) for x in range(0, w + 1, 4)]
    d.line(pts, fill=color, width=width)

def text_center(d, cx, y, s, font, fill):
    b = d.textbbox((0, 0), s, font=font)
    d.text((cx - (b[2] - b[0]) / 2 - b[0], y), s, font=font, fill=fill)

def save_indexed(img, path):
    img.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.NONE).save(path, optimize=True)

# icon0.png 128x128 (bubble icon)
icon = gradient(128, 128)
icon = glow(icon, 64, 64, 46, ACCENT, 120)
g = globe(84)
icon.alpha_composite(g, (22, 22))
save_indexed(icon, f"{OUT}/icon0.png")

# bg.png 840x500 (LiveArea background)
bg = gradient(840, 500)
bg = glow(bg, 660, 120, 220, ACCENT, 70)
bg = glow(bg, 150, 460, 200, ACCENT2, 45)
d = ImageDraw.Draw(bg)
for i, a in enumerate((26, 18, 12)):
    wave(d, 840, 400 + i * 22, a, (ACCENT[0], ACCENT[1], ACCENT[2], 60 - i * 15), 3)
bg.alpha_composite(globe(230), (560, 30))
save_indexed(bg, f"{OUT}/livearea/contents/bg.png")

# startup.png 280x158 (the "Start" gate tile)
st = gradient(280, 158)
st = glow(st, 60, 79, 70, ACCENT, 120)
st.alpha_composite(globe(84), (18, 37))
d = ImageDraw.Draw(st)
d.text((116, 46), "VitaSurf", font=ImageFont.truetype(BOLD, 32), fill=(236, 238, 245))
d.text((118, 90), "Simple, secure browsing", font=ImageFont.truetype(REG, 13), fill=(150, 160, 185))
d.rounded_rectangle((116, 114, 186, 118), 2, fill=ACCENT)
save_indexed(st, f"{OUT}/livearea/contents/startup.png")

# pic0.png 960x544 (shown while the app boots)
pic = gradient(960, 544)
pic = glow(pic, 480, 230, 200, ACCENT, 80)
pic.alpha_composite(globe(180), (390, 130))
d = ImageDraw.Draw(pic)
text_center(d, 480, 340, "VitaSurf", ImageFont.truetype(BOLD, 48), (236, 238, 245))
text_center(d, 480, 404, "Simple, secure browsing", ImageFont.truetype(REG, 20), (150, 160, 185))
save_indexed(pic, f"{OUT}/pic0.png")
print("done")
