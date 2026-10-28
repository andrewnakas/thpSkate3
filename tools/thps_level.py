#!/usr/bin/env python3
"""Extract a THPS N64 level's world geometry (every triangle, in world units, with its face word).

Layout, as the game's loaders read it (THPS64 decomp):
- Level file (pack section 0, index from the .psx name table D_800E8A58), loaded by func_80037DC8: 4 sub-files.
  [0] u32 n + n object records of 0x24 bytes: u32 flags, s32 pos[3] (x.12), s16 rot[3], u16 model, ...
  [1] u32 n + n model records of 0x18 bytes.   [3] u32 = index of the level's mesh pack (section 2).
- Mesh pack (pack section 2), loaded by func_800B5CA0: one mesh per model. A mesh is a 3-part container:
  [0] bounds (6 floats) + 2 floats.  [2] u32 count, u32 pad, count N64 Vtx (s16 x,y,z,flag, s16 s,t, u8 rgba).
  [1] batches; each batch is a 3-part container: [0] header (word 1 bit 0x8000 = raw display list, else packed),
      [1] packed display list (expanded by func_800B695C), [2] u32 count + one u32 face word per triangle.
- Packed display list bytes (func_800B695C): top bit set = triangle(s): 15 bits = three 5-bit cache slots, and
  a second triangle follows when the next byte also has the top bit; 0x20 = G_VTX (next n vertices of the mesh
  into slots m..m+n-1); 0x40 = G_MTX; 0x60 = G_MODIFYVTX; 0 = end.

  python tools/thps_level.py PACKDIR LEVEL OUT.obj [OUT.png]    LEVEL = SkMall, SkWare, ...
PACKDIR is the output of tools/thps_pack.py.
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thps_pack as tp  # noqa: E402

LEVEL_PSX_INDEX = {  # .psx name table (D_800E8A58), main level files
    'SkVans': 0, 'SkDown': 4, 'SkBurn': 10, 'SkSchl': 14, 'SkWare': 19, 'SkFway': 23, 'SkRos': 25,
    'SkJam': 49, 'SkMall': 55, 'SkSF': 62,
}


def container(b, base=0):
    n = struct.unpack_from('>I', b, base)[0]
    offs = struct.unpack_from('>%dI' % (n + 1), b, base + 4)
    return [(base + offs[i], base + offs[i + 1]) for i in range(n)]


def mesh_triangles(m):
    """Triangles of one mesh: list of ((v0, v1, v2) model-space s16 tuples, face word)."""
    parts = container(m)
    vstart, _ = parts[2]
    nverts = struct.unpack_from('>I', m, vstart)[0]
    verts = [struct.unpack_from('>hhh', m, vstart + 8 + 16 * i) for i in range(nverts)]
    out = []
    cursor = 0  # G_VTX loads the mesh's vertices in order
    b0, b1 = parts[1]
    for bs, be in container(m, b0):
        batch = container(m, bs)
        hdr = struct.unpack_from('>III', m, batch[0][0])
        if hdr[1] & 0x8000:
            continue  # raw display list (not seen in level meshes so far)
        fs = batch[2][0]
        nfaces = struct.unpack_from('>I', m, fs)[0]
        faces = struct.unpack_from('>%dI' % nfaces, m, fs + 4)
        cache = [None] * 32
        p = batch[1][0]
        tris = []
        while m[p] != 0:
            c = m[p]
            if c & 0x80:
                # resolve the slots now: a later G_VTX in the same batch reloads them
                a = (m[p] << 8) | m[p + 1]
                p += 2
                tris.append((cache[(a >> 10) & 31], cache[(a >> 5) & 31], cache[a & 31]))
                if m[p] & 0x80:
                    b = (m[p] << 8) | m[p + 1]
                    p += 2
                    tris.append((cache[(b >> 10) & 31], cache[(b >> 5) & 31], cache[b & 31]))
            elif (c & 0x60) == 0x20:
                a = (m[p] << 8) | m[p + 1]
                p += 2
                n = a & 31 or 32
                s = (a >> 5) & 31
                for k in range(n):
                    cache[s + k] = cursor + k
                cursor += n
            elif (c & 0x60) == 0x40:
                p += 2
            elif (c & 0x60) == 0x60:
                p += 5
            else:
                raise ValueError(f'bad display list byte {c:#x}')
        for t, (i0, i1, i2) in enumerate(tris):
            face = faces[t] if t < len(faces) else 0
            out.append(((verts[i0], verts[i1], verts[i2]), face))
    return out


def level_triangles(packdir, level):
    sec0 = tp.directory(open(os.path.join(packdir, 'sec0.bin'), 'rb').read())
    sec2 = tp.directory(open(os.path.join(packdir, 'sec2.bin'), 'rb').read())
    lf = sec0[LEVEL_PSX_INDEX[level]]
    subs = container(lf)
    objs_at = subs[0][0]
    nobj = struct.unpack_from('>I', lf, objs_at)[0]
    mesh_pack = struct.unpack_from('>I', lf, subs[3][0])[0]
    meshes = container(sec2[mesh_pack])
    pack = sec2[mesh_pack]
    cache = {}
    tris = []
    for i in range(nobj):
        r = objs_at + 4 + 0x24 * i
        flags, px, py, pz = struct.unpack_from('>Iiii', lf, r)
        model = struct.unpack_from('>H', lf, r + 0x16)[0]
        if model >= len(meshes):
            continue
        if model not in cache:
            a, b = meshes[model]
            cache[model] = mesh_triangles(pack[a:b])
        ox, oy, oz = px / 4096.0, py / 4096.0, pz / 4096.0
        for (v0, v1, v2), face in cache[model]:
            tris.append((tuple((v[0] + ox, v[1] + oy, v[2] + oz) for v in (v0, v1, v2)), face, i))
    return tris


def write_obj(tris, path):
    with open(path, 'w') as f:
        for (v0, v1, v2), face, obj in tris:
            for v in (v0, v1, v2):
                f.write('v %.3f %.3f %.3f\n' % (v[0], -v[1], v[2]))  # PSX y points down
        for i in range(len(tris)):
            f.write('f %d %d %d\n' % (3 * i + 1, 3 * i + 2, 3 * i + 3))


def render_top(tris, path, size=1024):
    from PIL import Image, ImageDraw
    xs = [v[0] for t in tris for v in t[0]]
    zs = [v[2] for t in tris for v in t[0]]
    ys = [v[1] for t in tris for v in t[0]]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    y0, y1 = min(ys), max(ys)
    s = (size - 8) / max(x1 - x0, z1 - z0)
    img = Image.new('RGB', (size, size), (20, 20, 28))
    d = ImageDraw.Draw(img)
    # far (low, large y) first so higher geometry draws on top
    for (v0, v1, v2), face, _ in sorted(tris, key=lambda t: -sum(v[1] for v in t[0])):
        h = (sum(v[1] for v in (v0, v1, v2)) / 3 - y0) / max(1e-6, y1 - y0)
        g = int(230 - 190 * h)
        pts = [(4 + (v[0] - x0) * s, 4 + (v[2] - z0) * s) for v in (v0, v1, v2)]
        d.polygon(pts, fill=(g, g, int(g * 0.9)), outline=(g // 2, g // 2, g // 2))
    img.save(path)
    return dict(x=(x0, x1), y=(y0, y1), z=(z0, z1))


def main():
    packdir, level, out = sys.argv[1], sys.argv[2], sys.argv[3]
    tris = level_triangles(packdir, level)
    write_obj(tris, out)
    print(f'{level}: {len(tris)} triangles -> {out}')
    if len(sys.argv) > 4:
        print(json.dumps(render_top(tris, sys.argv[4])))


if __name__ == '__main__':
    main()


def object_keys(packdir, level):
    """Trigger key of every level object (object record +0x1C): THPS runs the TRG node with this key when the
    skater crosses one of the object's trigger faces (func_800837C8 -> func_80061D7C)."""
    sec0 = tp.directory(open(os.path.join(packdir, 'sec0.bin'), 'rb').read())
    lf = sec0[LEVEL_PSX_INDEX[level]]
    at = container(lf)[0][0]
    n = struct.unpack_from('>I', lf, at)[0]
    return [struct.unpack_from('>I', lf, at + 4 + 0x24 * i + 0x1C)[0] for i in range(n)]


def write_cache(tris, keys, rail_lines, restart_points, path):
    """Binary level cache for the port (src/skate/level.cpp), little-endian: b'THL4', u32 triangle count, per
    triangle 9 f32 (THPS world units, PSX axes: y down) + u32 face word + u32 object index; then u32 object
    count and one u32 trigger key per object; then u32 rail count and per rail u32 point count + 3 f32 per point;
    then u32 restart count and per restart 3 f32 + u32 angle (4096ths of a turn about y).
    Face word bits (func_800837C8's filter): 0x10000 = no collision at all, 0x20000 = trigger only."""
    with open(path, 'wb') as f:
        f.write(b'THL4' + struct.pack('<I', len(tris)))
        for (v0, v1, v2), face, obj in tris:
            f.write(struct.pack('<9fII', *v0, *v1, *v2, face, obj))
        f.write(struct.pack('<I', len(keys)))
        f.write(struct.pack('<%dI' % len(keys), *keys))
        f.write(struct.pack('<I', len(rail_lines)))
        for line in rail_lines:
            f.write(struct.pack('<I', len(line)))
            for p in line:
                f.write(struct.pack('<3f', *p))
        f.write(struct.pack('<I', len(restart_points)))
        for x, y, z, a in restart_points:
            f.write(struct.pack('<3fI', x, y, z, a))


def export_all(packdir, outdir):
    os.makedirs(outdir, exist_ok=True)
    for level in LEVEL_PSX_INDEX:
        tris, flat = flatten_low_steps(level_triangles(packdir, level))
        tris, lips = set_back_lip_walls(tris)
        tris, before, after = smooth_solids(tris)
        print(f'{level}: {flat} low slab faces flattened, {lips} quarter-pipe lips opened above the vert')
        write_cache(tris, object_keys(packdir, level), rails(packdir, level), restarts(packdir, level),
                    os.path.join(outdir, level + '.thl'))
        print(f'{level}: {len(tris)} triangles, open solid edges {before} -> {after}')


LEVEL_TRG_INDEX = {  # .trg name table (D_800E8D18) indices = pack section 1 files
    'SkVans': 0, 'SkDown': 1, 'SkBurn': 2, 'SkJam': 3, 'SkMall': 4, 'SkRos': 5, 'SkSchl': 6, 'SkSF': 7, 'SkWare': 8,
}


def trg_nodes(packdir, level):
    """Nodes of a level's .trg (THPS1 PSX TRG, byte-swapped): b'GRT_', u32 version, u32 count, count offsets.
    A node is u16 type, u16 link count, the links (u16 node indices), then type-specific data; s32 fields are
    4-byte aligned in the file (func_800601A0 aligns its reads the same way). Returns [(type, links, pos|None)]."""
    if level not in LEVEL_TRG_INDEX:
        return []
    t = tp.directory(open(os.path.join(packdir, 'sec1.bin'), 'rb').read())[LEVEL_TRG_INDEX[level]]
    n = struct.unpack_from('>I', t, 8)[0]
    offs = struct.unpack_from('>%dI' % n, t, 12)
    nodes = []
    for o in offs:
        ty = struct.unpack_from('>H', t, o)[0]
        if ty == 0xFF:
            nodes.append((ty, [], None))
            continue
        if ty not in (8, 10, 11):  # only restarts and rails are needed; other types lay out fields differently
            nodes.append((ty, [], None))
            continue
        nl = struct.unpack_from('>H', t, o + 2)[0]
        links = list(struct.unpack_from('>%dH' % nl, t, o + 4))
        p = (o + 4 + 2 * nl + 3) & ~3
        pos = None
        if ty in (10, 11):  # RailPoint, RailDef: s32 x, y, z in world units
            pos = struct.unpack_from('>iii', t, p)
        if ty == 8:  # Restart: s32 x, y, z, then the facing (low 12 bits, 4096 = a turn about y), then its name
            x, y, z, a = struct.unpack_from('>iiiI', t, p)
            pos = (x, y, z, a & 0xFFF)
        nodes.append((ty, links, pos))
    return nodes


def rails(packdir, level):
    """Grind rails as polylines (THPS units): RailDef (11) / RailPoint (10) nodes linked to the next point."""
    nodes = trg_nodes(packdir, level)
    is_rail = lambda i: 0 <= i < len(nodes) and nodes[i][0] in (10, 11) and nodes[i][2] is not None
    nxt = {i: [l for l in nodes[i][1] if is_rail(l)] for i in range(len(nodes)) if is_rail(i)}
    incoming = {}
    for a, outs in nxt.items():
        for b in outs:
            incoming.setdefault(b, []).append(a)
    used = set()
    out = []
    def walk(start):
        line = [start]
        cur = start
        while True:
            outs = [b for b in nxt.get(cur, []) if (cur, b) not in used]
            if not outs:
                break
            b = outs[0]
            used.add((cur, b))
            line.append(b)
            if len(incoming.get(b, [])) != 1 or len(nxt.get(b, [])) != 1 or b == start:
                break
            cur = b
        return line
    starts = [i for i in nxt if len(incoming.get(i, [])) != 1 or len(nxt[i]) != 1]
    for s in starts + list(nxt):
        while any((s, b) not in used for b in nxt.get(s, [])):
            line = walk(s)
            if len(line) >= 2:
                out.append([nodes[i][2] for i in line])
    return out


def restarts(packdir, level):
    """Restart points: (x, y, z, angle) in world units, angle in 4096ths of a turn about y."""
    return [n[2] for n in trg_nodes(packdir, level) if n[0] == 8 and n[2] is not None]


def _weld(points, eps):
    """Map each point to a representative within eps (grid hashing)."""
    cell = {}
    rep = []
    out = []
    for p in points:
        key = tuple(int(round(c / eps)) for c in p)
        found = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for i in cell.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        q = rep[i]
                        if abs(q[0] - p[0]) <= eps and abs(q[1] - p[1]) <= eps and abs(q[2] - p[2]) <= eps:
                            found = i
                            break
                    if found is not None:
                        break
                if found is not None:
                    break
            if found is not None:
                break
        if found is None:
            found = len(rep)
            rep.append(tuple(p))
            cell.setdefault(key, []).append(found)
        out.append(found)
    return rep, out


def set_back_lip_walls(tris, height=100000.0, vert=30.0, margin=150.0, min_slope=50.0):
    """Many THPS quarter pipes end against a solid wall standing right on the lip (Warehouse's building walls).
    THPS's own physics launches straight up at a vert lip; Skate 3 instead keeps its wheels on the wall and rides
    up and along it. For collision only, the wall above such a lip is made non-solid from `vert` units above the lip
    up to `height`: the bottom `vert` units stay as a vertical lip extension, so the skater leaves straight up as in
    THPS. Nothing hidden is added (an earlier version moved the band back behind a small invisible deck, and boards
    carving along the coping caught on that deck). Only level coping lines on quarter-pipe tops of min_slope degrees
    or more count; the opening runs the wall's full height and `margin` units past each end of the lip (corners).
    tris: [(verts, face, obj)]. Returns (tris, number of lips opened)."""
    def sub(a, b): return [a[k] - b[k] for k in range(3)]
    def unit_normal(v):
        u, w = sub(v[1], v[0]), sub(v[2], v[0])
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        ln = math.sqrt(sum(x * x for x in n))
        return tuple(x / ln for x in n) if ln > 1e-9 else None

    solid = [i for i, t in enumerate(tris) if (t[1] & 0x30000) == 0]
    norms = {i: unit_normal(tris[i][0]) for i in solid}
    walls = [i for i in solid if norms[i] and abs(norms[i][1]) < 0.05]
    # walls by coarse grid cell of their horizontal extent, for the lip search
    cell = 512.0
    grid = {}
    for i in walls:
        xs = [v[0] for v in tris[i][0]]; zs = [v[2] for v in tris[i][0]]
        for gx in range(int(math.floor(min(xs) / cell)), int(math.floor(max(xs) / cell)) + 1):
            for gz in range(int(math.floor(min(zs) / cell)), int(math.floor(max(zs) / cell)) + 1):
                grid.setdefault((gx, gz), []).append(i)

    boxes = {}  # wall index -> [(u0, u1, band bottom y)]
    lips_found = []  # ((wall normal, plane offset), u0, u1, band bottom y)
    for i in solid:
        n = norms[i]
        if not n or not (0.01 < -n[1] < math.cos(math.radians(min_slope))):  # quarter-pipe tops, outward (up) side
            continue
        hn = math.hypot(n[0], n[2])
        hr = (n[0] / hn, n[2] / hn)
        v = tris[i][0]
        a, b = sorted(((v[k], v[(k + 1) % 3]) for k in range(3)), key=lambda e: e[0][1] + e[1][1])[0]  # highest edge
        span = math.hypot(b[0] - a[0], b[2] - a[2])
        if span < 8.0 or abs(a[1] - b[1]) > 0.03 * span + 1.0:  # a level coping line only
            continue
        yl = min(a[1], b[1])
        mx, mz = (a[0] + b[0]) / 2, (a[2] + b[2]) / 2
        for w in grid.get((int(math.floor(mx / cell)), int(math.floor(mz / cell))), []):
            wn = norms[w]
            hw = math.hypot(wn[0], wn[2])
            if (wn[0] * hr[0] + wn[2] * hr[1]) / hw < 0.7:
                continue
            wv = tris[w][0]
            if not all(abs(sum((p[k] - wv[0][k]) * wn[k] for k in range(3))) < 4.0 for p in (a, b)):
                continue
            if min(p[1] for p in wv) > yl - 50 or max(p[1] for p in wv) < yl - 2:
                continue  # the wall must rise above the lip and reach down to it
            t = (-wn[2] / hw, wn[0] / hw)  # horizontal tangent of the wall
            ue = sorted([a[0] * t[0] + a[2] * t[1], b[0] * t[0] + b[2] * t[1]])
            plane = (wn, sum(wv[0][k] * wn[k] for k in range(3)))
            lips_found.append((plane, ue[0] - margin, ue[1] + margin, yl - vert))
            break
    # Every solid wall triangle in a lip's wall plane gets that lip's box, not only the ones touching the lip: walls
    # are quads split into two triangles, and the one that meets the lip at a single corner covers most of the wall
    # above it.
    for w in walls:
        wn = norms[w]
        wv = tris[w][0]
        for (pn, pd), u0, u1, yb in lips_found:
            if wn[0] * pn[0] + wn[1] * pn[1] + wn[2] * pn[2] < 0.99:
                continue
            if not all(abs(sum(p[k] * pn[k] for k in range(3)) - pd) < 4.0 for p in wv):
                continue
            if min(p[1] for p in wv) > yb or max(p[1] for p in wv) < yb - height:
                continue
            hw = math.hypot(pn[0], pn[2])
            t = (-pn[2] / hw, pn[0] / hw)
            uw = [p[0] * t[0] + p[2] * t[1] for p in wv]
            if min(uw) < u1 and max(uw) > u0:
                boxes.setdefault(w, []).append((u0, u1, yb))

    def clip(poly, f):  # keep the part with f(p) <= 0
        out = []
        for k in range(len(poly)):
            p, q = poly[k], poly[(k + 1) % len(poly)]
            fp, fq = f(p), f(q)
            if fp <= 0:
                out.append(p)
            if (fp < 0 < fq) or (fq < 0 < fp):
                s = fp / (fp - fq)
                out.append([p[j] + s * (q[j] - p[j]) for j in range(3)])
        return out if len(out) >= 3 else []

    def fan(poly):
        return [[tuple(poly[0]), tuple(poly[k]), tuple(poly[k + 1])] for k in range(1, len(poly) - 1)]

    out = [t for k, t in enumerate(tris) if k not in boxes]
    lips = 0
    for w, bl in boxes.items():
        verts, face, obj = tris[w]
        wn = norms[w]
        hw = math.hypot(wn[0], wn[2])
        t = (-wn[2] / hw, wn[0] / hw)
        # merge boxes on the same lip height
        bl.sort()
        merged = []
        for u0, u1, yl in bl:
            if merged and u0 <= merged[-1][1] + 1.0 and abs(yl - merged[-1][2]) < 10.0:
                merged[-1] = (merged[-1][0], max(merged[-1][1], u1), min(merged[-1][2], yl))
            else:
                merged.append((u0, u1, yl))
        pieces = [[list(p) for p in verts]]
        for u0, u1, yl in merged:
            uf = lambda p: p[0] * t[0] + p[2] * t[1]
            rest = []
            for poly in pieces:
                cuts = [lambda p: u0 - uf(p), lambda p: uf(p) - u1, lambda p: (yl - height) - p[1], lambda p: p[1] - yl]
                inside = poly
                for c in cuts:  # outside pieces: each half-plane beyond the box, peeled off in turn
                    o = clip(inside, lambda p, c=c: -c(p))
                    if o:
                        rest.append(o)
                    inside = clip(inside, c)
                    if not inside:
                        break
                if inside:  # the band above the vert: kept for drawing-side bookkeeping but not solid
                    out += [(tri, face | 0x10000, obj) for tri in fan(inside)]
            pieces = rest
            lips += 1
        out += [(tri, face, obj) for poly in pieces for tri in fan(poly)]
    return out, lips


def flatten_low_steps(tris, max_step=12.0, cell=128.0):
    """Floor mats, boards and plates in THPS stand a few units proud of the floor with a vertical edge; THPS's
    skater rolls over them, Skate 3's wheels catch the edge at speed and throw the rider. Every near-level solid
    face with a near-level solid floor at most max_step units below it (PSX y points down) is lowered onto that
    floor, and the near-vertical solid faces no taller than max_step that stand on such a floor are dropped.
    Returns (triangles, faces changed)."""
    def nrm(t):
        a, b, c = t[0]
        u = [b[k] - a[k] for k in range(3)]
        v = [c[k] - a[k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(x * x for x in n))
        return tuple(x / ln for x in n) if ln > 1e-9 else (0.0, 0.0, 0.0)

    solid = lambda t: (t[1] & 0x30000) == 0
    level_faces = [t for t in tris if solid(t) and abs(nrm(t)[1]) > 0.97]
    grid = {}
    for k, t in enumerate(level_faces):
        xs = [v[0] for v in t[0]]
        zs = [v[2] for v in t[0]]
        for gx in range(int(min(xs) // cell), int(max(xs) // cell) + 1):
            for gz in range(int(min(zs) // cell), int(max(zs) // cell) + 1):
                grid.setdefault((gx, gz), []).append(k)

    def height_at(t, x, z):
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t[0]
        det = (bx - ax) * (cz - az) - (bz - az) * (cx - ax)
        if abs(det) < 1e-9:
            return None
        u = ((x - ax) * (cz - az) - (z - az) * (cx - ax)) / det
        w = ((bx - ax) * (z - az) - (bz - az) * (x - ax)) / det
        if u < -1e-4 or w < -1e-4 or u + w > 1 + 1e-4:
            return None
        return ay + u * (by - ay) + w * (cy - ay)

    def floor_below(x, y, z, exclude):
        best = None
        for k in grid.get((int(x // cell), int(z // cell)), ()):
            if k == exclude:
                continue
            h = height_at(level_faces[k], x, z)
            if h is not None and 0.5 < h - y <= max_step and (best is None or h < best):
                best = h  # the nearest floor below (larger y is lower)
        return best

    changed = 0
    out = []
    lowered = {}
    for k, t in enumerate(level_faces):
        cx = sum(v[0] for v in t[0]) / 3
        cy = sum(v[1] for v in t[0]) / 3
        cz = sum(v[2] for v in t[0]) / 3
        f = floor_below(cx, cy, cz, k)
        if f is not None:
            lowered[id(t)] = f - 0.5
    for t in tris:
        if id(t) in lowered:
            y = lowered[id(t)]
            out.append((tuple((v[0], max(v[1], y), v[2]) for v in t[0]), t[1], t[2]))
            changed += 1
            continue
        if solid(t) and abs(nrm(t)[1]) < 0.3:
            ys = [v[1] for v in t[0]]
            if max(ys) - min(ys) <= max_step:
                x = sum(v[0] for v in t[0]) / 3
                z = sum(v[2] for v in t[0]) / 3
                if floor_below(x, min(ys) - 0.25, z, -1) is not None or floor_below(x, max(ys) - 1.0, z, -1) is not None:
                    changed += 1
                    continue
        out.append(t)
    return out, changed


def subdivide_curves(rep, faces, iterations=2, max_angle=46.0, min_angle=2.0, max_slope=60.0):
    """Loop subdivision of the faces touching a shallow crease (min_angle..max_angle degrees between the two
    faces' normals). Faces steeper than max_slope degrees (the top of a quarter pipe, walls) are left as THPS
    built them: rounding them pulled the vertices under a ramp's lip outwards, curling the ramp past vertical into
    an overhang that Skate 3's wheels kept riding instead of launching. rep: vertex list (extended in place); faces: [([a, b, c], face, obj)]. Vertices on a sharp
    crease, an open edge or the region's border stay put, and edges on the border get plain midpoints, so the
    region still meets its neighbours along the same straight lines."""
    def normal(f):
        p = [rep[i] for i in f]
        u = [p[1][k] - p[0][k] for k in range(3)]
        v = [p[2][k] - p[0][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(x * x for x in n))
        return tuple(x / ln for x in n) if ln > 1e-9 else None

    flat = math.cos(math.radians(max_slope))  # THPS y points down: a face's up component is -normal.y
    for _ in range(iterations):
        edges = {}  # sorted edge -> [(face index, directed)]
        for fi, (f, _, _) in enumerate(faces):
            for e in range(3):
                a, b = f[e], f[(e + 1) % 3]
                edges.setdefault((min(a, b), max(a, b)), []).append((fi, (a, b)))
        norms = [normal(f) for f, _, _ in faces]

        def smooth_edge(key):
            uses = edges[key]
            if len(uses) != 2:
                return False
            (f1, d1), (f2, d2) = uses
            n1, n2 = norms[f1], norms[f2]
            if n1 is None or n2 is None:
                return False
            if d1 == d2:  # the two faces wind opposite ways: a fold of a two-sided sheet, keep it sharp
                return False
            c = sum(x * y for x, y in zip(n1, n2))
            ang = math.degrees(math.acos(max(-1.0, min(1.0, c))))
            return ang <= max_angle

        def crease(key):
            uses = edges[key]
            if len(uses) != 2:
                return False
            (f1, d1), (f2, d2) = uses
            n1, n2 = norms[f1], norms[f2]
            if n1 is None or n2 is None:
                return False
            if d1 == d2:
                return False
            c = sum(x * y for x, y in zip(n1, n2))
            ang = math.degrees(math.acos(max(-1.0, min(1.0, c))))
            return min_angle <= ang <= max_angle

        region = set()
        for key in edges:
            if crease(key):
                region.update(fi for fi, _ in edges[key] if norms[fi] is not None and -norms[fi][1] >= flat)
        if not region:
            break
        # vertex -> incident faces / edges
        vfaces, vedges = {}, {}
        for fi, (f, _, _) in enumerate(faces):
            for i in f:
                vfaces.setdefault(i, []).append(fi)
        for key in edges:
            for i in key:
                vedges.setdefault(i, []).append(key)
        interior_edge = {key: all(fi in region for fi, _ in edges[key]) and smooth_edge(key) for key in edges}
        moved = {}
        for i in {i for fi in region for i in faces[fi][0]}:
            if not all(fi in region for fi in vfaces[i]) or not all(interior_edge[k] for k in vedges[i]):
                continue
            nbrs = [k[0] if k[1] == i else k[1] for k in vedges[i]]
            n = len(nbrs)
            beta = 3.0 / 16.0 if n == 3 else 3.0 / (8.0 * n)
            moved[i] = tuple((1 - n * beta) * rep[i][k] + beta * sum(rep[j][k] for j in nbrs) for k in range(3))
        mid = {}

        def edge_point(a, b):
            key = (min(a, b), max(a, b))
            if key in mid:
                return mid[key]
            A, B = rep[a], rep[b]
            if interior_edge[key]:
                opp = []
                for fi, _ in edges[key]:
                    opp += [i for i in faces[fi][0] if i not in key]
                C, D = rep[opp[0]], rep[opp[1]]
                p = tuple(0.375 * (A[k] + B[k]) + 0.125 * (C[k] + D[k]) for k in range(3))
            else:
                p = tuple(0.5 * (A[k] + B[k]) for k in range(3))
            rep.append(p)
            mid[key] = len(rep) - 1
            return mid[key]

        out = []
        for fi, (f, face, obj) in enumerate(faces):
            if fi not in region:
                out.append((f, face, obj))
                continue
            a, b, c = f
            ab, bc, ca = edge_point(a, b), edge_point(b, c), edge_point(c, a)
            out += [([a, ab, ca], face, obj), ([ab, b, bc], face, obj), ([ca, bc, c], face, obj), ([ab, bc, ca], face, obj)]
        for i, p in moved.items():
            rep[i] = p
        faces = out
    return faces


def smooth_solids(tris, weld_eps=1.0, tj_eps=0.75, passes=4, curve_passes=0):
    """Collision smoothing for Skate 3's wheels: weld near-coincident vertices of the solid faces, then repair
    T-junctions (a vertex lying on another triangle's open edge splits that edge), so neighbouring faces share
    edges and skatecore sees continuous surfaces instead of sharp open edges. Non-solid faces pass unchanged.
    Returns (triangles, open_edges_before, open_edges_after)."""
    solid = [t for t in tris if (t[1] & 0x30000) == 0]
    other = [t for t in tris if (t[1] & 0x30000) != 0]
    pts = [v for t in solid for v in t[0]]
    rep, idx = _weld(pts, weld_eps)
    faces = []
    for k, t in enumerate(solid):
        a, b, c = idx[3 * k], idx[3 * k + 1], idx[3 * k + 2]
        if len({a, b, c}) == 3:
            faces.append(([a, b, c], t[1], t[2]))

    def open_edges(fs):
        cnt = {}
        for f, _, _ in fs:
            for i in range(3):
                e = tuple(sorted((f[i], f[(i + 1) % 3])))
                cnt[e] = cnt.get(e, 0) + 1
        return [e for e, n in cnt.items() if n == 1]

    before = len(open_edges(faces))
    # spatial hash of vertices for on-edge queries
    grid = {}
    G = 64.0
    for i, p in enumerate(rep):
        grid.setdefault(tuple(int(c // G) for c in p), []).append(i)

    def near_segment(a, b):
        lo = [min(rep[a][k], rep[b][k]) - tj_eps for k in range(3)]
        hi = [max(rep[a][k], rep[b][k]) + tj_eps for k in range(3)]
        for gx in range(int(lo[0] // G), int(hi[0] // G) + 1):
            for gy in range(int(lo[1] // G), int(hi[1] // G) + 1):
                for gz in range(int(lo[2] // G), int(hi[2] // G) + 1):
                    yield from grid.get((gx, gy, gz), ())

    def on_segment(i, a, b):
        p, A, B = rep[i], rep[a], rep[b]
        ab = [B[k] - A[k] for k in range(3)]
        L2 = sum(x * x for x in ab)
        if L2 < 1e-6:
            return None
        t = sum((p[k] - A[k]) * ab[k] for k in range(3)) / L2
        if t <= 1e-3 or t >= 1 - 1e-3:
            return None
        d2 = sum((A[k] + t * ab[k] - p[k]) ** 2 for k in range(3))
        return t if d2 <= tj_eps * tj_eps else None

    for _ in range(passes):
        oe = set(open_edges(faces))
        changed = False
        new_faces = []
        for f, face, obj in faces:
            split = None
            for e in range(3):
                a, b = f[e], f[(e + 1) % 3]
                if tuple(sorted((a, b))) not in oe:
                    continue
                lo = [min(rep[a][k], rep[b][k]) - tj_eps for k in range(3)]
                hi = [max(rep[a][k], rep[b][k]) + tj_eps for k in range(3)]
                cands = []
                for gx in range(int(lo[0] // G), int(hi[0] // G) + 1):
                    for gy in range(int(lo[1] // G), int(hi[1] // G) + 1):
                        for gz in range(int(lo[2] // G), int(hi[2] // G) + 1):
                            for i in grid.get((gx, gy, gz), ()):
                                if i in f:
                                    continue
                                t = on_segment(i, a, b)
                                if t is not None:
                                    cands.append((t, i))
                if cands:
                    split = (e, sorted(cands))
                    break
            if split is None:
                new_faces.append((f, face, obj))
                continue
            e, cands = split
            a, b, c = f[e], f[(e + 1) % 3], f[(e + 2) % 3]
            chain = [a] + [i for _, i in cands] + [b]
            for k in range(len(chain) - 1):
                new_faces.append(([chain[k], chain[k + 1], c], face, obj))
            changed = True
        faces = new_faces
        if not changed:
            break
    # Optional rounding of curved ramps (Loop subdivision next to shallow creases). Off by default: subdivision
    # moves vertices off THPS's surfaces (15-100 units in thousands of places), which the player meets as invisible
    # bumps and walls where the collision stands proud of what is drawn, and clipping where it sinks below it.
    faces = subdivide_curves(rep, faces, iterations=curve_passes)
    grid.clear()
    for i, p in enumerate(rep):
        grid.setdefault(tuple(int(c // G) for c in p), []).append(i)
    for _ in range(passes):
        oe = set(open_edges(faces))
        changed = False
        new_faces = []
        for f, face, obj in faces:
            split = None
            for e in range(3):
                a, b = f[e], f[(e + 1) % 3]
                if tuple(sorted((a, b))) not in oe:
                    continue
                cands = [(t, i) for i in near_segment(a, b) if i not in f
                         for t in [on_segment(i, a, b)] if t is not None]
                if cands:
                    split = (e, sorted(cands))
                    break
            if split is None:
                new_faces.append((f, face, obj))
                continue
            e, cands = split
            a, b, c = f[e], f[(e + 1) % 3], f[(e + 2) % 3]
            chain = [a] + [i for _, i in cands] + [b]
            for k in range(len(chain) - 1):
                new_faces.append(([chain[k], chain[k + 1], c], face, obj))
            changed = True
        faces = new_faces
        if not changed:
            break
    # vertical slivers no taller than a centimetre: seams between floor pieces, which would only catch wheels
    def sliver(f):
        p = [rep[i] for i in f]
        u = [p[1][k] - p[0][k] for k in range(3)]
        v = [p[2][k] - p[0][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        return abs(n[1]) / ln < 0.3 and max(q[1] for q in p) - min(q[1] for q in p) <= 1.0
    faces = [x for x in faces if not sliver(x[0])]
    after = len(open_edges(faces))
    out = [(tuple(rep[i] for i in f), face, obj) for f, face, obj in faces] + other
    return out, before, after
