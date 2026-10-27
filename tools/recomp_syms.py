#!/usr/bin/env python3
"""Inputs for N64Recomp (symbol-file mode): a "recomp ROM" with main stored uncompressed, and a symbols TOML.

  python tools/recomp_syms.py    (after tools/build_main.py and splat on config/boot.yaml + config/main.yaml)
-> work/recomp.z64   original ROM + main.bin appended at 0xC00000 (not a playable ROM; input for the recompiler)
   recomp/thps.syms.toml   sections boot + main with every function splat found (libultra named)
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.environ.get('THPS_ROM', "E:/Tony Hawk's Pro Skater (U) (V1.1) [!].z64")
MAIN_ROM = 0xC00000


def functions(asm_dir):
    out = []
    for dp, _, fs in os.walk(asm_dir):
        for f in fs:
            if not f.endswith('.s'):
                continue
            lines = open(os.path.join(dp, f)).read().splitlines()
            for i, line in enumerate(lines):
                m = re.match(r'nonmatching (\w+), 0x([0-9A-F]+)$', line)
                if m and any(l.startswith(f'glabel {m.group(1)}') for l in lines[i + 1:i + 3]):
                    addr = re.search(r'/\* [0-9A-F]+ ([0-9A-F]{8}) ', '\n'.join(lines[i + 1:i + 6]))
                    if addr:
                        out.append((int(addr.group(1), 16), int(m.group(2), 16), m.group(1)))
    return sorted(set(out))


def add_jal_targets(funcs, vram, data, named, extra_code=()):
    """Add a function for every jal target inside [vram, vram+len) that no function starts at."""
    import struct
    starts = {a for a, _, _ in funcs}
    targets = set()
    for base, blob in [(vram, data), *extra_code]:
        for i in range(0, len(blob) - 3, 4):
            w = struct.unpack_from('>I', blob, i)[0]
            if w >> 26 == 3:
                t = 0x80000000 | ((w & 0x3FFFFFF) << 2)
                if vram <= t < vram + len(data) and t not in starts:
                    targets.add(t)
    allstarts = sorted(starts | targets | {vram + len(data)})
    out = list(funcs)
    for t in sorted(targets):
        nxt = allstarts[allstarts.index(t) + 1]
        out.append((t, nxt - t, named.get(t, f'func_{t:08X}')))
    # a function that now contains a newly added start is cut there
    out.sort()
    fixed = []
    for k, (a, sz, n) in enumerate(out):
        if k + 1 < len(out) and a + sz > out[k + 1][0]:
            sz = out[k + 1][0] - a
        fixed.append((a, sz, n))
    return fixed


BRANCH_OPS = (4, 5, 6, 7, 20, 21, 22, 23)


def _branch_target(w, i):
    op = w >> 26
    if op in BRANCH_OPS or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19))             or (op == 17 and ((w >> 21) & 0x1F) == 8):
        off = (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF
        return i + 1 + off
    return None


NOT_R4300 = {'pref', 'movn', 'movz', 'movf', 'movt', 'madd', 'maddu', 'msub', 'msubu', 'mul', 'clz', 'clo'}


def looks_like_code(ws):
    """A split-off chunk is code only if every word is a valid instruction, jumps stay inside the game's
    code, and it ends in a return or jump (+ delay slot). Otherwise it is a constant pool."""
    import rabbitizer
    if len(ws) < 2:
        return False
    for w in ws:
        x = rabbitizer.Instruction(w)
        if not x.isValid() or x.getOpcodeName() in NOT_R4300 or w >> 26 in (0x12, 0x13, 0x32, 0x33, 0x36, 0x3A, 0x3E):
            return False
        if w >> 26 in (2, 3):
            t = 0x80000000 | ((w & 0x3FFFFFF) << 2)
            if not 0x80000400 <= t < 0x800F6460:
                return False
    end = ws[-2]
    return end == 0x03E00008 or end >> 26 == 2 or (end >> 26 == 0 and end & 0x3F == 8)


def add_gaps(funcs, lo, hi):
    """Gaps between known functions inside .text become candidates ('gap_'): split_leaves keeps only the pieces
    that look like code (splat sometimes swallows small functions after a constant pool as data)."""
    funcs = sorted(funcs)
    out, pos = list(funcs), lo
    for a, sz, _ in funcs + [(hi, 0, '')]:
        if a > pos + 4:
            out.append((pos, a - pos, f'gap_{pos:08X}'))
        pos = max(pos, a + sz)
    return sorted(out)


def split_leaves(funcs, vram, data):
    """Split a function after a `jr $ra` + delay slot that no earlier branch jumps past; the next valid
    instruction starts a new function (runs of small leaf functions with data words between them)."""
    import struct
    import rabbitizer
    out = []
    for a, sz, n in funcs:
        o = a - vram
        ws = [struct.unpack_from('>I', data, o + 4 * i)[0] for i in range(sz // 4)]
        start, reach, i = 0, 0, 0
        while i < len(ws):
            t = _branch_target(ws[i], i)
            if t is not None:
                reach = max(reach, t)
            if ws[i] == 0x03E00008 and i + 2 < len(ws) and reach <= i + 1:
                if (start == 0 and not n.startswith('gap_')) or looks_like_code(ws[start:i + 2]):
                    out.append((a + 4 * start, 4 * (i + 2 - start), n if start == 0 else f'func_{a + 4 * start:08X}'))
                j = i + 2
                while j < len(ws) and (not rabbitizer.Instruction(ws[j]).isValid() or ws[j] == 0):
                    j += 1
                start, reach, i = j, j, j
                continue
            i += 1
        if start < len(ws) and ((start == 0 and not n.startswith('gap_')) or looks_like_code(ws[start:])):
            out.append((a + 4 * start, 4 * (len(ws) - start), n if start == 0 else f'func_{a + 4 * start:08X}'))
    out = [(a, sz, f'func_{a:08X}' if n.startswith('gap_') else n) for a, sz, n in out]
    return out


def trim(funcs, vram, data):
    """End each function after its last `jr $ra` + delay slot unless a branch inside reaches further:
    KMC GCC leaves small constant pools between functions, which are not code."""
    import struct
    out = []
    for a, sz, n in funcs:
        o = a - vram
        ws = [struct.unpack_from('>I', data, o + 4 * i)[0] for i in range(sz // 4)]
        last = max((i for i, w in enumerate(ws) if w == 0x03E00008), default=None)
        if last is None or last + 2 >= len(ws):
            out.append((a, sz, n))
            continue
        reach = 0
        for i, w in enumerate(ws[:last + 2]):   # words after the last return may be data that decodes as a branch
            op = w >> 26
            if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19))                     or (op == 17 and ((w >> 21) & 0x1F) == 8):
                off = (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF
                reach = max(reach, i + 1 + off)
        end = max(last + 2, reach + 1)
        out.append((a, min(sz, 4 * end), n))
    return out


def merge_outside(funcs, vram, data):
    """If a function branches past its own end, it was over-split: absorb the following function(s)
    (and any gap) up to the branch target."""
    import struct
    funcs = sorted(funcs)
    changed = True
    while changed:
        changed = False
        for k, (a, sz, n) in enumerate(funcs):
            o = a - vram
            far = 0
            for i in range(sz // 4):
                t = _branch_target(struct.unpack_from('>I', data, o + 4 * i)[0], i)
                if t is not None:
                    far = max(far, 4 * t + 4)
            if far > sz:
                end = a + far
                j = k + 1
                while j < len(funcs) and funcs[j][0] < end:
                    end = max(end, funcs[j][0] + funcs[j][1])
                    j += 1
                funcs[k:j] = [(a, end - a, n)]
                changed = True
                break
    return funcs


def merge_fallthrough(funcs, vram, data):
    """A function that runs off its end into the next one was split at a prologue that is not its start: GCC
    varargs preambles (sw a1-a3 to the caller's home slots) and divides scheduled before `addiu sp`. Merge them.
    One that ends in a return/jump with a pool word after it is left alone."""
    import struct
    funcs = sorted(funcs)

    def ends(w):
        return w == 0x03E00008 or w >> 26 == 2 or (w >> 26 == 0 and w & 0x3F == 8) or w >> 16 == 0x1000

    k = 0
    while k + 1 < len(funcs):
        a, sz, n = funcs[k]
        ws = [struct.unpack_from('>I', data, a - vram + 4 * i)[0] for i in range(sz // 4)]
        if sz >= 8 and funcs[k + 1][0] == a + sz and not any(ends(w) for w in ws[-3:-1]):
            funcs[k:k + 2] = [(a, sz + funcs[k + 1][1], n)]
            continue
        k += 1
    return funcs


def section(name, rom, vram, size, funcs):
    s = [f'[[section]]\nname = "{name}"\nrom = 0x{rom:08X}\nvram = 0x{vram:08X}\nsize = 0x{size:X}\n\nfunctions = [']
    for a, sz, n in funcs:
        if vram <= a < vram + size:
            s.append(f'    {{ name = "{n}", vram = 0x{a:08X}, size = 0x{sz:X} }},')
    s.append(']\n')
    return '\n'.join(s)


def main():
    rom = open(ROM, 'rb').read()
    main_bin = open(os.path.join(ROOT, 'work', 'main.bin'), 'rb').read()
    out = bytearray(rom[:MAIN_ROM].ljust(MAIN_ROM, b'\xff')) + main_bin
    open(os.path.join(ROOT, 'work', 'recomp.z64'), 'wb').write(out)
    boot = functions(os.path.join(ROOT, 'asm_boot'))
    if not any(a == 0x80000400 for a, _, _ in boot):   # entry: sp, bss clear, jal 0x80000870 (splat calls it data)
        boot = sorted(boot + [(0x80000400, 0x68, 'entrypoint')])
    main_f = functions(os.path.join(ROOT, 'asm'))
    # boot functions the port implements itself (recomp/thps.toml `ignored`, port/src/glue.cpp): renamed so N64Recomp
    # does not hand them to the runtime, whose versions do not fit THPS (no OS_EVENT_PI, aborting raw DMA stub)
    # Boot's hand-written ERZ unpackers, as whole routines (the game calls them through 0x80000B08 to unpack its
    # files). Split at every label they could not be recompiled; as single functions every jump stays inside.
    UNPACKER = [(0x80000B08, 0xA0, 'erz_unpack_any'), (0x80000C40, 0xA8, 'erz2_unpack'), (0x80000CE8, 0x640, 'erz2_body'),
                (0x80001330, 0xC8, 'erz1_unpack'), (0x800013F8, 0x1B4, 'erz1_0'), (0x800015AC, 0xD8, 'erz1_1'),
                (0x80001684, 0x34, 'erz1_2'), (0x800016B8, 0x3C, 'erz1_3'), (0x800016F4, 0x24, 'erz1_4'),
                (0x80001718, 0x180, 'erz1_5'), (0x80001898, 0x24, 'erz1_6')]
    lo_u, hi_u = 0x80000B08, 0x800018BC
    boot = [f for f in boot if not lo_u <= f[0] < hi_u] + UNPACKER
    PORT_OWNED = {0x80002E00: 'thps_osSetEventMesg', 0x80004770: 'thps_osPiRawStartDma', 0x80004ED0: 'thps_osRecvMesg', 0x80005070: 'thps_osSendMesg', 0x80010A40: 'thps_osAiGetStatus'}
    # The game's own Controller Pak filesystem code, recompiled under thps_ names (the runtime's osPfs* are "no pak"
    # stubs); port/src/pak.cpp supplies only the lowest layer (PORT_OWNED below) against a 32 KB .mpk file.
    RECOMPILE_AS = {0x80008100: 'thps_osPfsAllocateFile', 0x80008E74: 'thps___osRepairPackId',
                    0x8000928C: 'thps___osCheckPackId', 0x80009424: 'thps___osGetId', 0x80009680: 'thps___osCheckId',
                    0x8000977C: 'thps___osPfsRWInode', 0x80009A9C: 'thps___osPfsSelectBank',
                    0x80009EA0: 'thps_osPfsDeleteFile', 0x8000A5AC: 'thps_osPfsReadWriteFile',
                    0x8000A9B0: 'thps_osPfsFileState', 0x8000ACA0: 'thps_osPfsFindFile', 0x8000B1D0: 'thps_osPfsFreeBlocks',
                    0x8000B320: 'thps_osPfsNumFiles', 0x8000B470: 'thps_osPfsInitPak', 0x8000B6E0: 'thps_osPfsChecker',
                    0x8000BDA4: 'thps_corrupted_init', 0x8000BF58: 'thps_corrupted'}
    PORT_OWNED.update({0x800088B0: 'thps___osContRamWrite', 0x80009B10: 'thps___osContRamRead',
                       0x8000C3A0: 'thps___osPfsGetStatus', 0x8000AE60: 'thps_osPfsIsPlug'})
    boot = [(a, sz, PORT_OWNED.get(a, RECOMPILE_AS.get(a, n))) for a, sz, n in boot]
    # every jal target is a function, even where splat saw data (libultra helpers like fcos)
    named = {}
    for line in open(os.path.join(ROOT, 'config', 'boot_symbols.txt')):
        m = re.match(r'(\w+) = 0x([0-9A-F]{8})', line)
        if m:
            named[int(m.group(2), 16)] = m.group(1)
    named.update(PORT_OWNED)
    named.update(RECOMPILE_AS)
    boot, main_f = add_jal_targets(boot, 0x80000400, rom[0x1000:0x13A10], named, extra_code=[(0x80016AC0 + 0xB900, main_bin[0xB900:0xAA360])]),         add_jal_targets(main_f, 0x80016AC0, main_bin, named, extra_code=[(0x80000400, rom[0x1000:0x13A10])])
    boot = sorted([f for f in boot if not lo_u <= f[0] < hi_u] + UNPACKER)
    # splat's sizes sometimes end inside a function: run each one to the next known start instead
    starts = sorted(main_f)
    text_end = 0x80016AC0 + 0xAA360
    main_f = [(a, (starts[k + 1][0] if k + 1 < len(starts) else text_end) - a, n) for k, (a, _, n) in enumerate(starts)
              if a < text_end]
    main_f = add_gaps(main_f, 0x800223C0, text_end)
    main_f = merge_outside(trim(split_leaves(main_f, 0x80016AC0, main_bin), 0x80016AC0, main_bin), 0x80016AC0, main_bin)
    main_f = merge_fallthrough(main_f, 0x80016AC0, main_bin)
    # COP2 (PSX GTE leftovers: the port emulates the GTE in C, e.g. func_80042D1C) cannot be recompiled. In the
    # recompiler's input only, each COP2-family instruction inside a function becomes `break 0x3F2`, so the
    # runtime stops at that exact address if the game ever reaches one. The decomp keeps the real bytes.
    import struct
    patched = []
    for a, sz, n in main_f:
        for o in range(a - 0x80016AC0, a - 0x80016AC0 + sz, 4):
            w = struct.unpack_from('>I', out, MAIN_ROM + o)[0]
            if w >> 26 in (0x12, 0x32, 0x36, 0x3A, 0x3E):
                struct.pack_into('>I', out, MAIN_ROM + o, (0x3F2 << 16) | 0x0D)
                patched.append(0x80016AC0 + o)
    open(os.path.join(ROOT, 'work', 'recomp.z64'), 'wb').write(out)
    open(os.path.join(ROOT, 'recomp', 'cop2_sites.txt'), 'w').write(''.join(f'0x{a:08X}' + chr(10) for a in patched))
    # signature-matched names inside main (n_audio, libmus, libc helpers, osVoice*) from config/n64sym.txt: only in the
    # recompiler's symbols, so N64Recomp can hand runtime-owned ones over (splat/Steward keep func_XXXXXXXX names)
    n64sym = {}
    for line in open(os.path.join(ROOT, 'config', 'n64sym.txt')):
        m = re.match(r'(\w+) = 0x([0-9A-F]{8});', line)
        # never a name the runtime would take over (osVoice*, rspboot...): in game code those are look-alike
        # matches of tiny functions, and handing them to the runtime breaks the game (osVoiceInit at 0x800A78E4)
        if m and int(m.group(2), 16) >= 0x80016AC0 and not m.group(1).startswith(('osVoice', 'rspboot')):
            n64sym[int(m.group(2), 16)] = m.group(1)
    main_f = [(a, sz, n64sym.get(a, n)) for a, sz, n in main_f]
    os.makedirs(os.path.join(ROOT, 'recomp'), exist_ok=True)
    with open(os.path.join(ROOT, 'recomp', 'thps.syms.toml'), 'w') as f:
        f.write('# generated by tools/recomp_syms.py: do not edit\n\n')
        f.write(section('boot', 0x1000, 0x80000400, 0x12A10, boot))
        f.write('\n' + section('main', MAIN_ROM, 0x80016AC0, len(main_bin), main_f))
    print(f'boot {len(boot)} functions, main {len(main_f)} functions, {len(patched)} COP2 sites -> break '
          f'(recomp/cop2_sites.txt) -> recomp/thps.syms.toml, work/recomp.z64')


if __name__ == '__main__':
    main()
