#!/usr/bin/env python3
"""Generate every icon of the product from assets/icon/source.png (the user's artwork, RGBA, transparent corners).

    D:\\Python3.8\\python.exe tools\\make-icons.py

Outputs
  assets/icon/app-1024.png, app.ico                         exe / window / installer icon (16..256 px)
  assets/icon/tray-{gray,amber,green,red}.ico               tray icons with a status dot (16..48 px)
  android/app/res/mipmap-*/ic_launcher.png                  legacy launcher icons
  android/app/res/mipmap-*/ic_launcher_foreground.png       adaptive icon foreground (the glyph on transparent)
  android/app/res/mipmap-anydpi-v26/ic_launcher.xml         adaptive icon
  android/app/res/values/ic_launcher_background.xml          its background colour (the artwork's dark)
  assets/icon/preview.png                                    contact sheet for a quick visual check
"""
import os
import sys

sys.path.insert(0, r'D:\Workspace\MatePadPaper\.pydeps')
import numpy as np
from PIL import Image, ImageDraw

ROOT = r'D:\Workspace\MatePadPaper'
SRC = os.path.join(ROOT, 'assets', 'icon', 'source.png')
OUT = os.path.join(ROOT, 'assets', 'icon')
RES = os.path.join(ROOT, 'android', 'app', 'res')
DARK = (38, 38, 40)

src = Image.open(SRC).convert('RGBA')


def resized(im, n):
    return im.resize((n, n), Image.LANCZOS)


# ---- PC icons -------------------------------------------------------------------------------------------------------------------
base = resized(src, 1024)
base.save(os.path.join(OUT, 'app-1024.png'))
sizes = [16, 20, 24, 28, 32, 40, 48, 64, 96, 128, 256]
base.save(os.path.join(OUT, 'app.ico'), sizes=[(s, s) for s in sizes])

STATE = {'gray': (150, 150, 150), 'amber': (240, 160, 0), 'green': (30, 190, 70), 'red': (225, 50, 50)}


def with_dot(color, n=512):
    im = resized(src, n).copy()
    d = ImageDraw.Draw(im)
    cx, cy, r = int(n * 0.76), int(n * 0.76), int(n * 0.21)
    d.ellipse([cx - r - n * 0.035, cy - r - n * 0.035, cx + r + n * 0.035, cy + r + n * 0.035], fill=(255, 255, 255, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color + (255,))
    return im


tray_sizes = [16, 20, 24, 32, 40, 48]
for name, col in STATE.items():
    big = with_dot(col)
    big.save(os.path.join(OUT, 'tray-%s.ico' % name), sizes=[(s, s) for s in tray_sizes])

# ---- Android --------------------------------------------------------------------------------------------------------------------
legacy = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}
fg = {'mdpi': 108, 'hdpi': 162, 'xhdpi': 216, 'xxhdpi': 324, 'xxxhdpi': 432}
for d, n in legacy.items():
    p = os.path.join(RES, 'mipmap-' + d)
    os.makedirs(p, exist_ok=True)
    resized(src, n).save(os.path.join(p, 'ic_launcher.png'))

# the glyph = everything that differs from the dark squircle (alpha from the luminance above the background), cropped and centred
a = np.asarray(src).astype(np.float32)
lum = a[..., :3].mean(axis=2)
alpha = np.clip((lum - DARK[0]) / (200.0 - DARK[0]), 0, 1) * (a[..., 3] / 255.0)      # the cream tablet screen must come out opaque
mask = alpha > 0.35          # solid parts only: the faint dark-on-dark noise must not stretch the crop box
ys, xs = np.where(mask)
x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
glyph = np.zeros((y1 - y0, x1 - x0, 4), np.float32)
glyph[..., :3] = a[y0:y1, x0:x1, :3]
glyph[..., 3] = alpha[y0:y1, x0:x1] * 255.0
glyph_im = Image.fromarray(glyph.astype(np.uint8), 'RGBA')
for d, n in fg.items():
    canvas = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    gw = int(n * 0.58)
    gh = int(gw * glyph_im.height / glyph_im.width)
    g = glyph_im.resize((gw, gh), Image.LANCZOS)
    canvas.paste(g, ((n - gw) // 2, (n - gh) // 2), g)
    p = os.path.join(RES, 'mipmap-' + d)
    canvas.save(os.path.join(p, 'ic_launcher_foreground.png'))
os.makedirs(os.path.join(RES, 'mipmap-anydpi-v26'), exist_ok=True)
with open(os.path.join(RES, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w', encoding='utf-8') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@color/ic_launcher_background"/>\n'
            '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
            '</adaptive-icon>\n')
os.makedirs(os.path.join(RES, 'values'), exist_ok=True)
with open(os.path.join(RES, 'values', 'ic_launcher_background.xml'), 'w', encoding='utf-8') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_background">#%02X%02X%02X</color>\n</resources>\n' % DARK)

# ---- preview --------------------------------------------------------------------------------------------------------------------
sheet = Image.new('RGBA', (1100, 330), (200, 200, 200, 255))
x = 10
for n in (256, 128, 64, 32, 16):
    t = resized(src, n)
    sheet.paste(t, (x, 10), t)
    x += n + 10
x = 10
for name in STATE:
    t = with_dot(STATE[name], 128)
    sheet.paste(t, (x, 190), t)
    t2 = with_dot(STATE[name], 32)
    sheet.paste(t2, (x + 100, 190 + 140), t2)
    x += 150
# the adaptive icon on a circle / squircle mask
ad = Image.new('RGBA', (216, 216), DARK + (255,))
g = Image.open(os.path.join(RES, 'mipmap-xhdpi', 'ic_launcher_foreground.png'))
ad.paste(g, (0, 0), g)
m = Image.new('L', (216, 216), 0)
ImageDraw.Draw(m).rounded_rectangle([24, 24, 192, 192], radius=44, fill=255)
sheet.paste(ad, (840, 10), m)
sheet.save(os.path.join(OUT, 'preview.png'))
print('icons written; glyph bbox', (x0, y0, x1, y1))
