#!/usr/bin/env python3
"""THPS skater rig helpers: per-bone mesh vertices (skater.PSX, mesh pack) and live bone matrices from RDRAM.

The skater model object sits at skater +0xF18; its draw (func_80084C5C) points RSP segment 1 at
+0xF18 + parity*0x600: up to 24 N64 Mtx (64 bytes, s15.16), one per bone, selected by the mesh display list.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thps_pack as tp  # noqa: E402
import thps_level as tl  # noqa: E402


def n64_mtx(b):
    """64-byte N64 Mtx -> 4x4 float rows (row-vector convention: v' = v * M)."""
    hi = struct.unpack_from('>16h', b, 0)
    lo = struct.unpack_from('>16H', b, 32)
    return [[hi[4 * i + j] + lo[4 * i + j] / 65536.0 for j in range(4)] for i in range(4)]


def bone_vertices(packdir, psx_index=78):
    sec0 = tp.directory(open(os.path.join(packdir, 'sec0.bin'), 'rb').read())
    sec2 = tp.directory(open(os.path.join(packdir, 'sec2.bin'), 'rb').read())
    lf = sec0[psx_index]
    subs = tl.container(lf)
    mp = struct.unpack_from('>I', lf, subs[3][0])[0]
    pack = sec2[mp]
    a, b = tl.container(pack)[0]
    m = pack[a:b]
    parts = tl.container(m)
    vs = parts[2][0]
    nv = struct.unpack_from('>I', m, vs)[0]
    verts = [struct.unpack_from('>hhh', m, vs + 8 + 16 * i) for i in range(nv)]
    cursor, cur, out = 0, None, {}
    for bs, _ in tl.container(m, parts[1][0]):
        batch = tl.container(m, bs)
        p = batch[1][0]
        while m[p] != 0:
            c = m[p]
            if c & 0x80:
                p += 2
                if m[p] & 0x80:
                    p += 2
            elif (c & 0x60) == 0x20:
                w = (m[p] << 8) | m[p + 1]
                p += 2
                n = w & 31 or 32
                out.setdefault(cur, []).extend(verts[cursor:cursor + n])
                cursor += n
            elif (c & 0x60) == 0x40:
                cur = m[p + 1]
                p += 2
            else:
                p += 5
    return out


def skater_triangles(packdir, psx_index=78):
    """Triangles of the skater mesh as (bone, (v0, v1, v2)) with bone-local s16 vertices."""
    sec0 = tp.directory(open(os.path.join(packdir, 'sec0.bin'), 'rb').read())
    sec2 = tp.directory(open(os.path.join(packdir, 'sec2.bin'), 'rb').read())
    lf = sec0[psx_index]
    subs = tl.container(lf)
    mp = struct.unpack_from('>I', lf, subs[3][0])[0]
    pack = sec2[mp]
    a, b = tl.container(pack)[0]
    m = pack[a:b]
    parts = tl.container(m)
    vs = parts[2][0]
    nv = struct.unpack_from('>I', m, vs)[0]
    verts = [struct.unpack_from('>hhh', m, vs + 8 + 16 * i) for i in range(nv)]
    cursor, cur, out = 0, None, []
    cache = [None] * 32
    for bs, _ in tl.container(m, parts[1][0]):
        batch = tl.container(m, bs)
        p = batch[1][0]
        while m[p] != 0:
            c = m[p]
            if c & 0x80:
                w = (m[p] << 8) | m[p + 1]
                p += 2
                tris = [w]
                if m[p] & 0x80:
                    tris.append((m[p] << 8) | m[p + 1])
                    p += 2
                for w in tris:
                    idx = ((w >> 10) & 31, (w >> 5) & 31, w & 31)
                    out.append(tuple(cache[i] for i in idx))
            elif (c & 0x60) == 0x20:
                w = (m[p] << 8) | m[p + 1]
                p += 2
                n = w & 31 or 32
                s = (w >> 5) & 31
                for k in range(n):
                    cache[s + k] = (cur, verts[cursor + k])
                cursor += n
            elif (c & 0x60) == 0x40:
                cur = m[p + 1]
                p += 2
            else:
                p += 5
    return out


CHARACTER_PSX = {  # .psx name table indices of the skater models (and their front-end variants)
    'hawk': 31, 'hawk_fe': 32, 'glif': 33, 'glif_FE': 34, 'BURNQ': 35, 'Burnq_FE': 36, 'Lasek': 37,
    'Lasek_FE': 38, 'muska': 39, 'muska_FE': 40, 'Rowley': 41, 'Rowley_F': 42, 'Rynld': 43, 'Rynld_FE': 44,
    'campb': 70, 'campb_fe': 71, 'FatCop': 72, 'FatCop_F': 73, 'Steamr': 74, 'Steamr_F': 75, 'Thomas': 76,
    'Thomas_F': 77, 'skater': 78, 'Skater_F': 79,
}


# THPS skeleton (all characters): parent of each bone, -1 for roots (pelvis; the board bones are separate).
PARENT = {3: -1, 10: 3, 11: 10, 18: 11, 14: 11, 13: 14, 12: 13, 17: 11, 16: 17, 15: 16,
          4: 3, 6: 4, 5: 6, 7: 3, 9: 7, 8: 9, 0: -1, 1: 0, 2: 0}


def joint_vertices(tris, a, b):
    """Skin vertices on the seam between bones a and b: those of triangles that use both, per side."""
    sa, sb = set(), set()
    for tri in tris:
        bones = {x[0] for x in tri}
        if a in bones and b in bones:
            for bone, v in tri:
                (sa if bone == a else sb if bone == b else set()).add(v)
    return sorted(sa), sorted(sb)


def export_rigs(packdir, path):
    """work/levels/rigs.thr for the port's retargeting (port/src/skate_bridge.cpp), little-endian: b'THR3',
    u32 count, then per character mesh: u32 mesh pack index (what the game stores at model +0xC08), u32 bone
    count, per bone the bone-local centroid of its vertices (3 f32, 0 if the bone has none); then u32 seam
    count and per seam u32 parent, u32 child, u32 na, u32 nb, na parent-local and nb child-local vertices; then
    per bone u32 vertex count and its bone-local vertices."""
    sec0 = tp.directory(open(os.path.join(packdir, 'sec0.bin'), 'rb').read())
    entries = []
    for name, idx in CHARACTER_PSX.items():
        lf = sec0[idx]
        subs = tl.container(lf)
        mp = struct.unpack_from('>I', lf, subs[3][0])[0]
        groups = {}
        for tri in skater_triangles(packdir, idx):
            for b, v in tri:
                groups.setdefault(b, set()).add(v)
        nb = max(groups) + 1 if groups else 0
        cents = []
        for b in range(nb):
            vs = groups.get(b, ())
            cents.append([sum(v[j] for v in vs) / len(vs) if vs else 0.0 for j in range(3)])
        tris = skater_triangles(packdir, idx)
        joints = []
        for child, parent in PARENT.items():
            if parent < 0:
                continue
            va, vb = joint_vertices(tris, parent, child)
            joints.append((parent, child, va, vb))
        entries.append((mp, cents, joints, groups))
        print(f'{name}: mesh pack {mp}, {nb} bones, seams ' +
              ' '.join(f'{p}-{c}:{len(a)}+{len(b)}' for p, c, a, b in joints))
    with open(path, 'wb') as f:
        f.write(b'THR3' + struct.pack('<I', len(entries)))
        for mp, cents, joints, groups in entries:
            f.write(struct.pack('<II', mp, len(cents)))
            for c in cents:
                f.write(struct.pack('<3f', *c))
            f.write(struct.pack('<I', len(joints)))
            for parent, child, va, vb in joints:
                f.write(struct.pack('<IIII', parent, child, len(va), len(vb)))
                for v in va + vb:
                    f.write(struct.pack('<3f', *v))
            for b in range(len(cents)):  # every bone's vertices (bone-local), for seams on rigid-piece models
                vs = sorted(groups.get(b, ()))
                f.write(struct.pack('<I', len(vs)))
                for v in vs:
                    f.write(struct.pack('<3f', *v))
