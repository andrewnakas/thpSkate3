#!/usr/bin/env python3
"""Contact sheet of screenshots: python tools/sheet.py OUT.png IMG... (each scaled to 480 wide, 2 per row)."""
import sys
from PIL import Image, ImageDraw

out, paths = sys.argv[1], sys.argv[2:]
ims = [Image.open(p).convert('RGB') for p in paths]
w = 480
ims = [im.resize((w, int(im.height * w / im.width))) for im in ims]
h = max(im.height for im in ims)
cols = 2
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (w * cols, (h + 16) * rows), (0, 0, 0))
d = ImageDraw.Draw(sheet)
for i, (im, p) in enumerate(zip(ims, paths)):
    x, y = (i % cols) * w, (i // cols) * (h + 16)
    sheet.paste(im, (x, y + 16))
    d.text((x + 4, y + 2), p.split('/')[-1], fill=(255, 255, 0))
sheet.save(out)
