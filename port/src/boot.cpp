// THPS boot, replacing the cartridge's boot main (0x80000870, marked `ignored` in recomp/thps.toml).
//
// On hardware, boot main runs a few libultra calls, then reads the chunk table at ROM 0x13B14 and unpacks 14
// ERZ blocks (Edge of Reality's renamed RNC ProPack, method 1) back to back at 0x80016AC0 with hand-written
// MIPS unpackers that cannot be recompiled. This does the same in C++ from the player's ROM, then enters the
// game's main (0x800C0480) exactly as boot does. Mirrors tools/build_main.py and tools/erz.py.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

extern "C" {
void osInitialize_recomp(uint8_t* rdram, recomp_context* ctx);
void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx);
void thps_osSetEventMesg(uint8_t* rdram, recomp_context* ctx);
void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx);
void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx);
void thps_main(uint8_t* rdram, recomp_context* ctx);
}

namespace {

constexpr uint32_t kChunkTable = 0x13B14;
constexpr uint32_t kMainVram = 0x80016AC0;
constexpr uint32_t kBssStart = 0x800F6460, kBssEnd = 0x800FC8C8;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void put_byte(uint8_t* rdram, uint32_t vaddr, uint8_t b) { rdram[(vaddr ^ 3) - 0x80000000u] = b; }

// RNC method 1 (Simon Tatham's dernc): little-endian 16-bit words, LSB first; p is the last loaded word.
struct Bits {
    const uint8_t* d; size_t n; size_t p; uint32_t buf; int cnt;
    uint32_t word(size_t at) const { return (at < n ? d[at] : 0) | ((at + 1 < n ? d[at + 1] : 0) << 8); }
    Bits(const uint8_t* d_, size_t n_, size_t p_) : d(d_), n(n_), p(p_), buf(word(p_)), cnt(16) {}
    void advance(int k) {
        buf >>= k; cnt -= k;
        if (cnt < 16) { p += 2; buf |= word(p) << cnt; cnt += 16; }
    }
    uint32_t read(int k) { uint32_t v = buf & ((1u << k) - 1); advance(k); return v; }
    void fix() { cnt -= 16; buf &= (1u << cnt) - 1; buf |= word(p) << cnt; cnt += 16; }
};

struct Leaf { uint32_t code; int len; int value; };

std::vector<Leaf> table(Bits& bs) {
    std::vector<Leaf> out;
    const int num = int(bs.read(5));
    if (num == 0) return out;
    std::vector<int> lens(num);
    for (int& l : lens) l = int(bs.read(4));
    uint32_t code = 0;
    for (int k = 1; k <= 16; k++) {
        for (int i = 0; i < num; i++) {
            if (lens[i] == k) {
                uint32_t r = 0, c = code;
                for (int b = 0; b < k; b++) { r = (r << 1) | (c & 1); c >>= 1; }
                out.push_back({ r, k, i });
                code++;
            }
        }
        code <<= 1;
    }
    return out;
}

bool huf(const std::vector<Leaf>& t, Bits& bs, uint32_t& v) {
    for (const Leaf& l : t) {
        if ((bs.buf & ((1u << l.len) - 1)) == l.code) {
            bs.advance(l.len);
            v = uint32_t(l.value);
            if (v >= 2) v = (1u << (v - 1)) | bs.read(int(v) - 1);
            return true;
        }
    }
    return false;
}

bool erz1(std::span<const uint8_t> rom, uint32_t off, std::vector<uint8_t>& out) {
    const uint8_t* h = rom.data() + off;
    if (std::memcmp(h, "ERZ\x01", 4) != 0) return false;
    const uint32_t usz = be32(h + 4);
    out.clear();
    out.reserve(usz);
    Bits bs(rom.data(), rom.size(), off + 18);
    bs.advance(2);
    while (out.size() < usz) {
        auto raw = table(bs), dist = table(bs), len = table(bs);
        int chunks = int(bs.read(16));
        for (;;) {
            uint32_t n;
            if (!huf(raw, bs, n)) return false;
            if (n) {
                if (bs.p + n > rom.size()) return false;
                out.insert(out.end(), rom.begin() + bs.p, rom.begin() + bs.p + n);
                bs.p += n;
                bs.fix();
            }
            if (--chunks <= 0) break;
            uint32_t posn, cnt;
            if (!huf(dist, bs, posn) || !huf(len, bs, cnt)) return false;
            posn += 1; cnt += 2;
            if (posn > out.size()) return false;
            for (uint32_t i = 0; i < cnt; i++) out.push_back(out[out.size() - posn]);
        }
    }
    out.resize(usz);
    return true;
}

void call(void (*fn)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx,
          int32_t a0 = 0, int32_t a1 = 0, int32_t a2 = 0) {
    ctx->r4 = gpr(a0); ctx->r5 = gpr(a1); ctx->r6 = gpr(a2);
    fn(rdram, ctx);
}

}  // namespace

extern "C" void func_80000870(uint8_t* rdram, recomp_context* ctx) {   // not RECOMP_FUNC: that is "extern inline" (weak) and would be dropped
    // The libultra setup boot main does first (see the disassembly at 0x80000870).
    call(osInitialize_recomp, rdram, ctx);
    call(osViBlack_recomp, rdram, ctx, 1);
    call(osAiSetFrequency_recomp, rdram, ctx, 0x5622);
    call(osCreateMesgQueue_recomp, rdram, ctx, int32_t(0x80011C10), int32_t(0x80011C28), 1);
    call(thps_osSetEventMesg, rdram, ctx, 0xE, int32_t(0x80011C10), 0);

    for (uint32_t a = kBssStart; a < kBssEnd; a++) put_byte(rdram, a, 0);

    const std::span<const uint8_t> rom = recomp::get_rom();
    const uint32_t n = be32(rom.data() + kChunkTable);

    // What boot main copies before unpacking (see 0x80000900..0x80000968): the asset area's PI address (ROM 0x13B10,
    // 0xB0061E26 in V1.1) to 0x80011C2C, which the game's file code reads, and the chunk table to 0x803C0000.
    auto put_word = [&](uint32_t vaddr, uint32_t w) { for (int b = 0; b < 4; b++) put_byte(rdram, vaddr + b, uint8_t(w >> (24 - 8 * b))); };
    put_word(0x80011C2C, be32(rom.data() + 0x13B10));
    // osMemSize: the runtime reports 8 MB, so THPS finds an Expansion Pak (its high-res/extra-memory path).
    // THPS_4MB=1 reports a stock 4 MB console instead ("Expansion Pak not found").
    if (const char* e = std::getenv("THPS_4MB"); e != nullptr && *e == '1') put_word(0x80000318, 0x00400000);
    for (uint32_t k = 0; k < n + 2; k++) put_word(0x803C0000 + 4 * k, be32(rom.data() + kChunkTable + 4 * k));
    uint32_t dst = kMainVram;
    std::vector<uint8_t> chunk;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t o = kChunkTable + be32(rom.data() + kChunkTable + 4 + 4 * i);
        const uint32_t end = kChunkTable + be32(rom.data() + kChunkTable + 8 + 4 * i);
        if (std::memcmp(rom.data() + o, "ERZ\x01", 4) == 0) {
            if (!erz1(rom, o, chunk)) {
                std::fprintf(stderr, "[thps] boot: chunk %u at ROM 0x%X does not unpack\n", i, o);
                std::abort();
            }
        } else {
            chunk.assign(rom.begin() + o, rom.begin() + end);   // raw chunk (none in V1.1, but boot allows it)
        }
        for (size_t k = 0; k < chunk.size(); k++) put_byte(rdram, dst + uint32_t(k), chunk[k]);
        dst += 0x10000;
    }
    std::printf("[thps] boot: unpacked %u chunks of main to 0x%08X-0x%08X; entering 0x800C0480\n", n, kMainVram,
                kMainVram + (n - 1) * 0x10000 + uint32_t(chunk.size()));
    thps_main(rdram, ctx);
}
