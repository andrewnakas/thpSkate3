#!/usr/bin/env python3
"""THPS N64 asset pack reader.

The ROM holds a pack at 0x61E26: u32 section count, then section offsets (relative to the pack, one extra
entry for the end). Each section is u32 chunk count + chunk offsets; every chunk is one ERZ block that unpacks
to 16 KiB (the last one may be shorter). Concatenated, a section is a flat image, like a PSX CD track.

  python tools/thps_pack.py ROM OUTDIR     write OUTDIR/sec<N>.bin for every chunked section

Uses the ERZ unpacker of the THPS64 decomp (THPS64 env var, default ~/thps64-claude).
"""
import os
import struct
import sys

THPS64 = os.environ.get('THPS64', os.path.expanduser('~/thps64-claude'))
sys.path.insert(0, os.path.join(THPS64, 'tools'))
import erz  # noqa: E402

PACK = 0x61E26


def sections(rom):
    n = struct.unpack_from('>I', rom, PACK)[0]
    offs = struct.unpack_from('>%dI' % (n + 1), rom, PACK + 4)
    return [(PACK + offs[i], PACK + offs[i + 1]) for i in range(n)]


def chunks(rom, start):
    cnt = struct.unpack_from('>I', rom, start)[0]
    offs = struct.unpack_from('>%dI' % (cnt + 1), rom, start + 4)
    return [start + offs[i] for i in range(cnt)]


def section_image(rom, start):
    """Unpack a chunked section; None if it is not one (section 5 holds raw audio tables)."""
    out = bytearray()
    for c in chunks(rom, start):
        if rom[c:c + 3] != b'ERZ':
            return None
        data, ok = erz.unpack(rom, c)
        if not ok:
            raise ValueError(f'ERZ CRC mismatch at {c:#x}')
        out += data
    return bytes(out)


def main():
    rom = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for i, (a, b) in enumerate(sections(rom)):
        img = section_image(rom, a)
        if img is None:
            print(f'sec{i}: not chunked, skipped')
            continue
        open(os.path.join(outdir, f'sec{i}.bin'), 'wb').write(img)
        print(f'sec{i}: {len(img):#x} bytes')


if __name__ == '__main__':
    main()


def directory(img):
    """Files of a section image: u32 count, count+1 offsets (last = end)."""
    cnt = struct.unpack_from('>I', img, 0)[0]
    offs = struct.unpack_from('>%dI' % (cnt + 1), img, 4)
    return [img[offs[i]:offs[i + 1]] for i in range(cnt)]


MAIN_BASE = 0x80016AC0


def name_tables(main):
    """{name, index} tables in the game's data: runs of (pointer to a C string, small int), ended by index -1."""
    def cstr(p):
        o = p - MAIN_BASE
        if not 0 <= o < len(main):
            return None
        e = main.find(b'\0', o)
        s = main[o:e]
        if not 3 <= len(s) <= 40 or any(c < 0x20 or c > 0x7e for c in s):
            return None
        return s.decode()
    tables = []
    o = 0
    while o + 8 <= len(main):
        run = []
        p = o
        while p + 8 <= len(main):
            ptr, idx = struct.unpack_from('>II', main, p)
            s = cstr(ptr)
            if s is None or '.' not in s or idx > 0x400:
                break
            run.append((s, idx))
            p += 8
        if len(run) >= 3:
            tables.append((MAIN_BASE + o, run))
            o = p
        else:
            o += 4
    return tables
