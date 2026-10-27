#!/usr/bin/env python3
"""Render the THPS skater mesh posed by the model-space bone matrices in an RDRAM dump (skater +0x214 array).
   python tools/skel_view.py DUMP OUT.png [bone matrices source: 'anim' (default) | 'n64' (the +0xF18 buffer)]"""
import struct
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import thps_skel as ts
from rdram import Ram
from PIL import Image, ImageDraw

r = Ram(sys.argv[1])
sk = r.u32(0x800DE7A8)
src = sys.argv[3] if len(sys.argv) > 3 else 'anim'
if src == 'anim':
    arr = r.u32(sk + 0x214)
    M = [[list(struct.unpack_from('>4f', r.bytes(arr + 64 * b + 16 * i, 16))) for i in range(4)] for b in range(19)]
else:
    M = [ts.n64_mtx(r.bytes(sk + 0xF18 + 64 * b, 64)) for b in range(19)]
tris = ts.skater_triangles('work/pack')
def xf(bv):
    b, v = bv
    m = M[b]
    return [sum(v[k] * m[k][j] for k in range(3)) + m[3][j] for j in range(3)]
W = [[xf(x) for x in t] for t in tris]
allp = [p for t in W for p in t]
c = [sum(p[j] for p in allp) / len(allp) for j in range(3)]
span = max(max(abs(p[j] - c[j]) for p in allp) for j in range(3)) or 1
s = 240 / span
img = Image.new('RGB', (1000, 560), (20, 20, 25))
d = ImageDraw.Draw(img)
cols = [(255, 80, 80), (80, 255, 80), (80, 80, 255), (255, 255, 80), (255, 80, 255), (80, 255, 255), (255, 160, 60),
        (160, 60, 255), (60, 255, 160), (255, 255, 255), (200, 120, 120), (120, 200, 120), (120, 120, 200),
        (200, 200, 120), (200, 120, 200), (120, 200, 200), (150, 150, 150), (255, 200, 200), (200, 255, 200)]
for view, ox in ((0, 0), (2, 500)):
    order = sorted(range(len(W)), key=lambda i: -sum(p[2 - view] for p in W[i]))
    for i in order:
        b = tris[i][0][0]
        d.polygon([(ox + 250 + (p[view] - c[view]) * s, 280 + (p[1] - c[1]) * s) for p in W[i]], fill=cols[b % 19])
    for b in range(19):
        d.text((ox + 250 + (M[b][3][view] - c[view]) * s, 280 + (M[b][3][1] - c[1]) * s), str(b), fill=(0, 0, 0))
    d.text((ox + 10, 10), 'x-y (front)' if view == 0 else 'z-y (side)', fill=(255, 255, 0))
img.save(sys.argv[2])
