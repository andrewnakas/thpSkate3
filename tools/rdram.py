#!/usr/bin/env python3
"""Read THPS state out of RDRAM dumps (port F9 / THPS_DUMP_AT; big-endian, 8 MB from 0x80000000).

  python tools/rdram.py DUMP...      summary of each dump (mode, level, skater pointer, position, state)
"""
import struct
import sys


class Ram:
    def __init__(self, path):
        self.b = open(path, 'rb').read()

    def off(self, a):
        return (a & 0x7FFFFFFF) if a >= 0x80000000 else a

    def u32(self, a):
        return struct.unpack_from('>I', self.b, self.off(a))[0]

    def s32(self, a):
        return struct.unpack_from('>i', self.b, self.off(a))[0]

    def s16(self, a):
        return struct.unpack_from('>h', self.b, self.off(a))[0]

    def u16(self, a):
        return struct.unpack_from('>H', self.b, self.off(a))[0]

    def f32(self, a):
        return struct.unpack_from('>f', self.b, self.off(a))[0]

    def vec3i(self, a):
        return struct.unpack_from('>iii', self.b, self.off(a))

    def bytes(self, a, n):
        o = self.off(a)
        return self.b[o:o + n]


SKATER = 0x800DE7A8   # current skater object (include/types/object.h)
LEVEL = 0x800C0E20    # level index into D_800D5C1C
MODE = 0x800DE7A0     # game mode (7 = some special mode in the trigger code)


def summary(r):
    sk = r.u32(SKATER)
    out = {'mode': r.s32(MODE), 'level': r.s32(LEVEL), 'skater': hex(sk)}
    if 0x80000000 <= sk < 0x80800000:
        pos = r.vec3i(sk + 4)
        out['pos_units'] = tuple(round(v / 4096, 1) for v in pos)
        out['state_82C'] = r.s32(sk + 0x82C)
        out['vel_8D0'] = r.vec3i(sk + 0x8D0)
        m = struct.unpack_from('>9h', r.bytes(sk + 0x884, 18))
        out['mtx_884'] = [round(v / 4096, 3) for v in m]
    return out


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(p, summary(Ram(p)))
