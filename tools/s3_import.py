#!/usr/bin/env python3
"""Import Skate 3's stock data from your own retail Xbox 360 disc image into the skatecore pack.

  python3 tools/s3_import.py list  SKATE3.iso [PATTERN]        list files on the disc (PATTERN: substring)
  python3 tools/s3_import.py check SKATE3.iso PACK              compare PACK's stock/data files with the disc's
  python3 tools/s3_import.py build SKATE3.iso BASE_PACK OUT     write OUT: BASE_PACK with every stock/data file
                                                                 replaced by the disc's own copy

The pack (skatecore-cpp's SKPK1 format: b'SKPK1\\0\\0\\0', u32 count, then per file u32 name length, name, u64 size,
bytes; little-endian) keeps Skate 3's own files under assets/private/stock/data/..., mirroring the disc's data/ tree.
The disc is read directly (XDVDFS, the Xbox game file system), including the EB .big archives inside it
(RefPack-compressed); nothing is unpacked to disk.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
# Where the game partition starts: XGD2 / XGD3 discs, a "game partition only" rip, or a plain XISO.
PARTITION_OFFSETS = (0xFD90000, 0x2080000, 0x18300000, 0)
STOCK = 'assets/private/stock/'


class Xiso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.base = None
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(20) == MAGIC:
                self.base = off
                break
        if self.base is None:
            raise ValueError(f'{path}: no XDVDFS volume found (not an Xbox 360 disc image?)')
        self.f.seek(self.base + 32 * SECTOR + 20)
        self.root_sector, self.root_size = struct.unpack('<II', self.f.read(8))

    def read(self, sector, size, offset=0):
        self.f.seek(self.base + sector * SECTOR + offset)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=''):
        """Yield (path, sector, size, is_dir) for every entry (directory tables are binary trees)."""
        if sector is None:
            sector, size = self.root_sector, self.root_size
        if size == 0:
            return
        table = self.read(sector, size)
        stack, seen = [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, start, length, attr, nlen = struct.unpack_from('<HHIIBB', table, off)
            if left == 0xFFFF:  # padding
                continue
            name = table[off + 14:off + 14 + nlen].decode('latin-1')
            path = prefix + name
            if attr & 0x10:
                yield path, start, length, True
                yield from self.walk(start, length, path + '/')
            else:
                yield path, start, length, False
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)

    def files(self):
        return {p.lower(): (p, s, n) for p, s, n, d in self.walk() if not d}


def refpack(src):
    """EA RefPack (QFS) decompression: header 0x10FB (or flag variants) + big-endian uncompressed size."""
    flags = src[0]
    at = 2
    width = 4 if flags & 0x80 else 3
    if flags & 0x01:
        at += width  # compressed size
    size = int.from_bytes(src[at:at + width], 'big')
    at += width
    out = bytearray()
    n = len(src)
    while at < n:
        b0 = src[at]
        if b0 < 0x80:
            b1 = src[at + 1]
            plain, length, offset = b0 & 3, ((b0 & 0x1C) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1
            at += 2
        elif b0 < 0xC0:
            b1, b2 = src[at + 1], src[at + 2]
            plain, length, offset = (b1 >> 6) & 3, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
            at += 3
        elif b0 < 0xE0:
            b1, b2, b3 = src[at + 1], src[at + 2], src[at + 3]
            plain, length = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
            at += 4
        elif b0 < 0xFC:
            plain, length, offset = ((b0 & 0x1F) << 2) + 4, 0, 0
            at += 1
        else:
            plain, length, offset = b0 & 3, 0, 0
            at += 1
            out += src[at:at + plain]
            break
        out += src[at:at + plain]
        at += plain
        start = len(out) - offset
        for k in range(length):  # overlapping copies repeat bytes
            out.append(out[start + k])
    if len(out) != size:
        raise ValueError(f'RefPack: got {len(out)} bytes, header says {size}')
    return bytes(out)


class EbBig:
    """Skate 3's 'EB' version 3 .big archives (big-endian): header, 16-byte entries (offset >> alignment shift,
    stored size, size, name hash), one compressed flag byte per entry, then the name table (per entry a 2-byte
    folder index and the name, in fixed-size records) and, 16-byte aligned, the folder table (fixed-size paths)."""

    def __init__(self, data):
        if data[:4] != b'EB\x00\x03':
            raise ValueError('not an EB v3 .big')
        self.data = data
        count = struct.unpack_from('>I', data, 4)[0]
        shift = data[0x0A]
        names_at = struct.unpack_from('>I', data, 0x0C)[0]
        name_len, folder_len, folders = data[0x14], data[0x15], struct.unpack_from('>H', data, 0x16)[0]
        flags = data[0x30 + 16 * count:0x30 + 17 * count]
        folders_at = (names_at + count * name_len + 15) & ~15
        folder = [data[folders_at + k * folder_len:folders_at + (k + 1) * folder_len].split(b'\0')[0].decode('latin-1')
                  for k in range(folders)]
        self.entries = {}
        for i in range(count):
            off, stored, size, _ = struct.unpack_from('>IIII', data, 0x30 + 16 * i)
            rec = data[names_at + i * name_len:names_at + (i + 1) * name_len]
            fi = struct.unpack_from('>H', rec)[0]
            name = rec[2:].split(b'\0')[0].decode('latin-1')
            path = (folder[fi] + '/' + name) if fi < len(folder) and folder[fi] else name
            self.entries[path.lower()] = (off << shift, stored, size, flags[i])

    def read(self, key):
        off, stored, size, compressed = self.entries[key]
        raw = self.data[off:off + stored]
        return refpack(raw) if compressed and raw[1:2] == b'\xfb' else raw[:size]


class Disc:
    """Every file of a Skate 3 disc, loose or inside its .big archives, by lower-case path."""

    def __init__(self, iso):
        self.xiso = Xiso(iso)
        self.loose = self.xiso.files()
        self.bigs = {}
        self.where = {}
        for key, (p, s, n) in self.loose.items():
            if key.endswith('.big') and '/audio/' not in key and '/movies/' not in key:
                head = self.xiso.read(s, 4)
                if head == b'EB\x00\x03':
                    self.where.update({k: key for k in self._index(key, s, n)})

    def _index(self, key, sector, size):
        big = EbBig(self.xiso.read(sector, size))
        self.bigs[key] = big
        return big.entries.keys()

    def get(self, path):
        key = path.lower()
        if key in self.loose:
            _, s, n = self.loose[key]
            return self.xiso.read(s, n)
        if key in self.where:
            return self.bigs[self.where[key]].read(key)
        return None


def read_pack(path):
    b = open(path, 'rb').read()
    if b[:8] != b'SKPK1\0\0\0':
        raise ValueError(f'{path}: not an SKPK1 pack')
    n = struct.unpack_from('<I', b, 8)[0]
    at, out = 12, []
    for _ in range(n):
        ln = struct.unpack_from('<I', b, at)[0]
        name = b[at + 4:at + 4 + ln].decode()
        at += 4 + ln
        size = struct.unpack_from('<Q', b, at)[0]
        at += 8
        out.append((name, b[at:at + size]))
        at += size
    return out


def write_pack(path, entries):
    with open(path + '.tmp', 'wb') as f:
        f.write(b'SKPK1\0\0\0' + struct.pack('<I', len(entries)))
        for name, data in entries:
            nb = name.encode()
            f.write(struct.pack('<I', len(nb)) + nb + struct.pack('<Q', len(data)) + data)
    os.replace(path + '.tmp', path)


def disc_copy(disc, name):
    """The disc's bytes for a pack entry under assets/private/stock/, or None."""
    if not name.startswith(STOCK):
        return None
    return disc.get(name[len(STOCK):])


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, iso = argv[1], argv[2]
    disc = Disc(iso)
    if cmd == 'list':
        pat = argv[3].lower() if len(argv) > 3 else ''
        for key, (p, s, n) in sorted(disc.loose.items()):
            if pat in key:
                print(f'{n:12d}  {p}')
        for key, big in sorted(disc.where.items()):
            if pat in key:
                print(f'{disc.bigs[big].entries[key][2]:12d}  {key}  (in {big})')
        print(f'{len(disc.loose)} files on the disc, {len(disc.where)} inside its .big archives '
              f'(partition at {disc.xiso.base:#x})')
        return 0
    pack = read_pack(argv[3])
    stock = [(n, d) for n, d in pack if n.startswith(STOCK)]
    if cmd == 'check':
        same = differ = missing = 0
        for name, data in stock:
            got = disc_copy(disc, name)
            if got is None:
                missing += 1
                print('not on disc:', name)
            elif got == data:
                same += 1
            else:
                differ += 1
                print(f'differs: {name} (pack {len(data)} bytes, disc {len(got)})')
        print(f'{len(stock)} stock files: {same} identical on the disc, {differ} differ, {missing} not on the disc')
        return 0 if differ == 0 and missing == 0 else 1
    if cmd == 'build':
        out, replaced, kept = [], 0, []
        for name, data in pack:
            got = disc_copy(disc, name)
            if got is not None:
                out.append((name, got))
                replaced += 1
            else:
                out.append((name, data))
                if name.startswith(STOCK):
                    kept.append(name)
        write_pack(argv[4], out)
        print(f'{argv[4]}: {len(out)} files, {replaced} stock files from the disc')
        for name in kept:
            print('  kept from the base pack (not on the disc):', name)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
