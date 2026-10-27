// Virtual Controller Pak for THPS: the lowest layer only.
//
// The game's own libultra filesystem (osPfsInitPak, osPfsFindFile, osPfsReadWriteFile ...) is recompiled under
// thps_ names (tools/recomp_syms.py RECOMPILE_AS); everything it does ends in 32-byte block reads/writes, which land
// here against a 32 KB image kept in <settings>/thps64.mpk (the raw layout emulators call .mpk). Written from the
// libultra pfs format, not from another port's code.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "recomp.h"

namespace thps {
std::filesystem::path settings_directory();   // src/main.cpp
}

namespace {

// THPS_TRACE_PAK=1: log every low-level pak call (first 300)
bool trace_pak() { static const bool on = std::getenv("THPS_TRACE_PAK") != nullptr; return on; }
void tlog(const char* what, int channel, uint32_t block) {
    static int n = 0;
    if (trace_pak() && n++ < 300) std::fprintf(stderr, "[thps] pak %s ch%d block 0x%03X\n", what, channel, block);
}

constexpr size_t kPakSize = 0x8000;            // one bank, 128 pages of 256 bytes
std::array<uint8_t, kPakSize> g_pak{};
std::mutex g_pak_lock;
bool g_loaded = false;

std::filesystem::path pak_path() {
    if (const char* e = std::getenv("THPS_MPK")) return e;
    return thps::settings_directory() / "thps64.mpk";
}

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

// A freshly formatted pak, as __osRepairPackId and the libultra pfs layout expect: four copies of the pack ID in
// page 0, the inode table (page 1) and its backup (page 2) with the five system pages reserved and every file page
// free, and an empty note directory (pages 3-4).
void format(std::array<uint8_t, kPakSize>& pak) {
    pak.fill(0);
    uint8_t id[32] = {};
    put16(id + 0, 0xFFFF); put16(id + 2, 0xFFFF);                    // repaired = 0xFFFFFFFF
    const uint32_t random = 0x54485053;                               // 'THPS'
    for (int i = 0; i < 4; i++) id[4 + i] = uint8_t(random >> (24 - 8 * i));
    for (int i = 8; i < 24; i++) id[i] = uint8_t(0x11 * (i - 7));    // serial
    put16(id + 24, 0x0001);                                           // deviceid (bit 0 set: a real pak)
    id[26] = 1;                                                       // banks
    id[27] = 0;                                                       // version
    uint16_t sum = 0;
    for (int i = 0; i < 28; i += 2) sum = uint16_t(sum + ((id[i] << 8) | id[i + 1]));
    put16(id + 28, sum);
    put16(id + 30, uint16_t(0xFFF2 - sum));
    for (int block : { 1, 3, 4, 6 }) std::memcpy(&pak[block * 32], id, 32);

    for (int page : { 1, 2 }) {
        uint8_t* inode = &pak[page * 256];
        for (int e = 5; e < 128; e++) put16(inode + 2 * e, 0x0003);  // PFS_PAGE_NOT_USED
        uint32_t s = 0;
        for (int b = 5 * 2; b < 256; b++) s += inode[b];              // __osSumcalc over pages 5..127
        inode[1] = uint8_t(s);                                        // stored in inode_page[0].page
    }
}

void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(pak_path(), std::ios::binary);
    if (f.read(reinterpret_cast<char*>(g_pak.data()), kPakSize) && f.gcount() == static_cast<std::streamsize>(kPakSize)) {
        std::fprintf(stderr, "[thps] controller pak: %s\n", pak_path().string().c_str());
        return;
    }
    format(g_pak);
    std::fprintf(stderr, "[thps] controller pak: new formatted pak at %s\n", pak_path().string().c_str());
}

void save() {
    std::error_code ec;
    std::filesystem::create_directories(pak_path().parent_path(), ec);
    std::ofstream f(pak_path(), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(g_pak.data()), kPakSize);
}

}  // namespace

extern "C" {

// s32 __osContRamRead(OSMesgQueue* mq, int channel, u16 address, u8* buffer): one 32-byte block.
void thps___osContRamRead(uint8_t* rdram, recomp_context* ctx) {
    const int channel = static_cast<int32_t>(ctx->r5);
    const uint32_t block = static_cast<uint32_t>(ctx->r6) & 0xFFFF;
    const gpr buffer = ctx->r7;
    tlog("read ", channel, block);
    std::lock_guard lock{ g_pak_lock };
    load();
    for (int i = 0; i < 32; i++) {
        const size_t at = size_t(block) * 32 + i;
        MEM_B(i, buffer) = channel == 0 && at < kPakSize ? static_cast<int8_t>(g_pak[at]) : 0;
    }
    ctx->r2 = channel == 0 ? 0 : 1;   // PFS_ERR_NOPACK on the other ports
}

// s32 __osContRamWrite(OSMesgQueue* mq, int channel, u16 address, u8* buffer, int force)
void thps___osContRamWrite(uint8_t* rdram, recomp_context* ctx) {
    const int channel = static_cast<int32_t>(ctx->r5);
    const uint32_t block = static_cast<uint32_t>(ctx->r6) & 0xFFFF;
    const gpr buffer = ctx->r7;
    tlog("write", channel, block);
    if (channel != 0) {
        ctx->r2 = 1;
        return;
    }
    std::lock_guard lock{ g_pak_lock };
    load();
    if (size_t(block) * 32 + 32 <= kPakSize) {   // above 0x8000 is the bank/rumble register space: ignored
        for (int i = 0; i < 32; i++) g_pak[size_t(block) * 32 + i] = static_cast<uint8_t>(MEM_B(i, buffer));
        save();
    }
    ctx->r2 = 0;
}

// s32 __osPfsGetStatus(OSMesgQueue* mq, int channel): a pak is always present in port 1.
void thps___osPfsGetStatus(uint8_t*, recomp_context* ctx) {
    tlog("status", static_cast<int32_t>(ctx->r5), 0);
    ctx->r2 = static_cast<int32_t>(ctx->r5) == 0 ? 0 : 1;
}

// s32 osPfsIsPlug(OSMesgQueue* mq, u8* pattern): bit per port with a pak.
void thps_osPfsIsPlug(uint8_t* rdram, recomp_context* ctx) {
    tlog("isplug", 0, 0);
    MEM_B(0, ctx->r5) = 1;
    ctx->r2 = 0;
}

}  // extern "C"
