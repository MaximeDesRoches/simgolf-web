#!/usr/bin/env python3
# Draws the desktop app's icon (a flag on a green, original art) and writes
# native/host/macos/SimGolf.icns and native/host/windows/simgolf.ico.
# Needs Pillow; the .icns step needs macOS's iconutil.
import os, shutil, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')

def draw(size):
    s = 1024
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # rounded square: sky to fairway
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle((72, 72, s - 72, s - 72), radius=200, fill=255)
    bg = Image.new('RGBA', (s, s))
    bd = ImageDraw.Draw(bg)
    for y in range(s):
        t = y / s
        if t < 0.58:
            c = (int(120 + 60 * t), int(190 + 40 * t), 245)
        else:
            u = (t - 0.58) / 0.42
            c = (int(70 - 30 * u), int(160 - 50 * u), int(70 - 30 * u))
        bd.line([(0, y), (s, y)], fill=c + (255,))
    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # green (putting surface) ellipse and hole
    d.ellipse((220, 600, 820, 820), fill=(96, 196, 92, 255))
    d.ellipse((470, 690, 560, 730), fill=(20, 40, 20, 255))
    # pole and flag
    d.rectangle((505, 230, 525, 712), fill=(245, 245, 240, 255))
    d.polygon([(525, 235), (775, 315), (525, 400)], fill=(220, 40, 45, 255))
    # ball
    shadow = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse((300, 735, 420, 775), fill=(0, 0, 0, 90))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(8)))
    d.ellipse((300, 640, 410, 750), fill=(252, 252, 252, 255), outline=(200, 200, 200, 255), width=4)
    for (x, y) in [(330, 670), (360, 662), (338, 700), (372, 694), (352, 728), (386, 722)]:
        d.ellipse((x - 7, y - 7, x + 7, y + 7), fill=(222, 222, 222, 255))
    return img.resize((size, size), Image.LANCZOS)

def main():
    big = draw(1024)
    iconset = tempfile.mkdtemp(suffix='.iconset')
    for n in (16, 32, 128, 256, 512):
        draw(n).save(os.path.join(iconset, f'icon_{n}x{n}.png'))
        draw(n * 2).save(os.path.join(iconset, f'icon_{n}x{n}@2x.png'))
    out = os.path.join(ROOT, 'native/host/macos/SimGolf.icns')
    if shutil.which('iconutil'):
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', out], check=True)
    shutil.rmtree(iconset)
    big.save(os.path.join(ROOT, 'native/host/windows/simgolf.ico'),
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, 'native/host/icon-256.png'))

main()
