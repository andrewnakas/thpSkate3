#!/usr/bin/env python3
"""Build N64Recomp's input ROM without the THPS64 decomp's asm_boot tree.

Same bytes as THPS64 tools/recomp_syms.py writes: the original ROM up to 0xC00000, then the unpacked game code
(work/main.bin), with every COP2 instruction listed in recomp/cop2_sites.txt replaced by `break 0x3F2`.
recomp/thps.syms.toml is taken as committed.

  python tools/make_recomp_rom.py ROM     -> work/recomp.z64
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_ROM = 0xC00000
MAIN_VRAM = 0x80016AC0


def main():
    rom = open(sys.argv[1], 'rb').read()
    main_bin = open(os.path.join(ROOT, 'work', 'main.bin'), 'rb').read()
    out = bytearray(rom[:MAIN_ROM].ljust(MAIN_ROM, b'\xff')) + main_bin
    sites = [int(l, 16) for l in open(os.path.join(ROOT, 'recomp', 'cop2_sites.txt')) if l.strip()]
    for a in sites:
        o = MAIN_ROM + a - MAIN_VRAM
        w = struct.unpack_from('>I', out, o)[0]
        assert w >> 26 in (0x12, 0x32, 0x36, 0x3A, 0x3E), hex(a)
        struct.pack_into('>I', out, o, (0x3F2 << 16) | 0x0D)
    open(os.path.join(ROOT, 'work', 'recomp.z64'), 'wb').write(out)
    print(f'work/recomp.z64: {len(out):#x} bytes, {len(sites)} COP2 sites patched')


if __name__ == '__main__':
    main()
