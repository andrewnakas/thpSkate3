// THPS audio through mupen64plus-rsp-hle's "naudio" alist interpreter (third_party/rsp-hle, GPL-2.0-or-later: see
// third_party/rsp-hle/LICENSES before distributing a build that includes it).
//
// THPS ships n_aspMain (ucode_data + 0x10 == 0x0000127C, rsp-hle's "naudio (many games)"). The RSPRecomp'd copy of
// it (recomp/aspMain.thps.toml) runs every task but mixes silence; this HLE path is the known-good reference, chosen
// with THPS_AUDIO_HLE=1 (src/callbacks.cpp).

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "librecomp/rsp.hpp"

extern "C" {
#include "hle_internal.h"
void alist_process_naudio(struct hle_t* hle);

void HleVerboseMessage(void*, const char*, ...) {}
void HleInfoMessage(void*, const char*, ...) {}
void HleErrorMessage(void*, const char* message, ...) {
    va_list args;
    va_start(args, message);
    std::fprintf(stderr, "[thps] rsp-hle: ");
    std::vfprintf(stderr, message, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}
void HleWarnMessage(void*, const char* message, ...) {
    va_list args;
    va_start(args, message);
    std::fprintf(stderr, "[thps] rsp-hle warning: ");
    std::vfprintf(stderr, message, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}
void HleCheckInterrupts(void*) {}
void HleProcessDlistList(void*) {}
void HleProcessAlistList(void*) {}
void HleProcessRdpList(void*) {}
void HleShowCFB(void*) {}
int HleForwardTask(void*) { return 0; }
void rsp_break(struct hle_t*, unsigned int) {}
}

// librecomp's DMEM (librecomp/src/rsp.cpp): it has already loaded the task at 0xFC0 and the ucode data at 0.
extern uint8_t dmem[0x1000];

namespace {
hle_t g_hle{};
uint8_t g_imem[0x1000];
unsigned int g_regs[32];
}

RspExitReason naudio_hle_run(uint8_t* rdram, uint32_t ucode_addr) {
    (void)ucode_addr;
    if (g_hle.dram == nullptr) {
        g_hle.imem = g_imem;
        unsigned int** regs[] = { &g_hle.mi_intr, &g_hle.sp_mem_addr, &g_hle.sp_dram_addr, &g_hle.sp_rd_length,
                                  &g_hle.sp_wr_length, &g_hle.sp_status, &g_hle.sp_dma_full, &g_hle.sp_dma_busy,
                                  &g_hle.sp_pc, &g_hle.sp_semaphore, &g_hle.dpc_start, &g_hle.dpc_end,
                                  &g_hle.dpc_current, &g_hle.dpc_status, &g_hle.dpc_clock, &g_hle.dpc_bufbusy,
                                  &g_hle.dpc_pipebusy, &g_hle.dpc_tmem };
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) *regs[i] = &g_regs[i];
        g_hle.hle_aud = 1;
    }
    g_hle.dram = rdram;
    g_hle.dmem = dmem;
    // THPS_AUDIO_OPS=1: cumulative opcode counts over every task (is SETVOL ever sent?)
    static const bool ops = std::getenv("THPS_AUDIO_OPS") != nullptr;
    static uint64_t cum[64] = {}, n_tasks = 0;
    if (ops) {
        const uint32_t data = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x30) & 0x00FFFFFF;   // OSTask.data_ptr
        const uint32_t size = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x34);
        for (uint32_t off = 0; off + 8 <= size && off < 0x4000; off += 8) {
            cum[(*reinterpret_cast<uint32_t*>(rdram + data + off) >> 24) & 0x3F]++;
        }
        if (++n_tasks % 600 == 0) {
            std::fprintf(stderr, "[thps] hle cumulative ops after %llu tasks:", static_cast<unsigned long long>(n_tasks));
            for (int i = 0; i < 64; i++) if (cum[i]) std::fprintf(stderr, " %02X:%llu", i, static_cast<unsigned long long>(cum[i]));
            std::fprintf(stderr, "\n");
        }
    }
    alist_process_naudio(&g_hle);
    if (ops) {   // the list's last SAVEBUFF is the output buffer handed to osAiSetNextBuffer: anything in it?
        const uint32_t data = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x30) & 0x00FFFFFF;
        const uint32_t size = *reinterpret_cast<uint32_t*>(dmem + 0xFC0 + 0x34);
        uint32_t last_dst = 0, last_w1 = 0;
        for (uint32_t off = 0; off + 8 <= size && off < 0x4000; off += 8) {
            const uint32_t w1 = *reinterpret_cast<uint32_t*>(rdram + data + off);
            if (((w1 >> 24) & 0x3F) == 0x06) { last_w1 = w1; last_dst = *reinterpret_cast<uint32_t*>(rdram + data + off + 4) & 0x00FFFFFF; }
        }
        static uint64_t seen = 0, nonzero_tasks = 0;
        int nz = 0;
        for (uint32_t k = 0; k < 0x2E0 && last_dst; k += 4) nz += *reinterpret_cast<uint32_t*>(rdram + last_dst + k) != 0;
        nonzero_tasks += nz != 0;
        if (++seen % 600 == 0) {
            std::fprintf(stderr, "[thps] hle output: last SAVEBUFF %08X -> %06X, %d/184 nonzero words now; %llu/%llu tasks non-silent\n",
                         last_w1, last_dst, nz, static_cast<unsigned long long>(nonzero_tasks), static_cast<unsigned long long>(seen));
        }
    }
    return RspExitReason::Broke;
}
