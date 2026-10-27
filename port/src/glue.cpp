// Small pieces the Pilotwings-derived glue expects, specialised for THPS.
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "thps/callbacks.h"
#include "thps/glue.h"

namespace thps {

namespace { uint8_t* g_rdram = nullptr; }

// Scripted input for test runs: THPS_PRESS="A@6,START@12.5,DDOWN@14" holds each button for 0.25 s at that many
// seconds after the first poll. Player 1 only; the stick stays centred.
namespace {
struct Press { uint16_t mask; double at; };
std::vector<Press> load_presses() {
    std::vector<Press> out;
    const char* e = std::getenv("THPS_PRESS");
    if (e == nullptr) return out;
    static const std::pair<const char*, uint16_t> names[] = {
        {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"DUP", 0x0800}, {"DDOWN", 0x0400},
        {"DLEFT", 0x0200}, {"DRIGHT", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CUP", 0x0008}, {"CDOWN", 0x0004},
        {"CLEFT", 0x0002}, {"CRIGHT", 0x0001}};
    std::string spec = e;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t at = item.find('@');
        if (at != std::string::npos) {
            const std::string name = item.substr(0, at);
            for (const auto& [n, m] : names) {
                if (name == n) out.push_back({ m, std::atof(item.c_str() + at + 1) });
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}
uint16_t scripted_buttons() {
    static const std::vector<Press> presses = load_presses();
    static const auto start = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    uint16_t b = 0;
    for (const Press& p : presses) {
        if (t >= p.at && t < p.at + 0.25) b |= p.mask;
    }
    return b;
}
}  // namespace

void input_script_state(uint16_t* buttons, float* x, float* y) { *buttons = scripted_buttons(); *x = 0.0f; *y = 0.0f; }
void input_script_state(int player, uint16_t* buttons, float* x, float* y) {
    *buttons = player == 0 ? scripted_buttons() : 0; *x = 0.0f; *y = 0.0f;
}
void set_rdram_base(uint8_t* rdram) { g_rdram = rdram; }
uint8_t* rdram_base() { return g_rdram; }
void poll_game_state() {}

// No base-recomp patches yet (boot main is replaced by an `ignored` function in src/boot.cpp instead).
void register_patches() {}
void register_patched_addresses() {}

}  // namespace thps

// libultra internals that the runtime does not provide, called from boot functions that are not named yet (so
// N64Recomp recompiled them instead of handing them to the runtime). They touch hardware the runtime emulates at a
// higher level; the Controller Pak CRCs only matter to osPfs*, which libultra_stubs.cpp reports as "no pak".
extern "C" {
static void ret0(recomp_context* ctx) { ctx->r2 = 0; }
void __osAiDeviceBusy_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osContAddressCrc_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osContDataCrc_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osDequeueThread_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osResetGlobalIntMask_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osSetGlobalIntMask_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osSiRawStartDma_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osTimerInterrupt_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osSiGetAccess_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osViInit_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osViGetCurrentContext_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }   // only the replaced VI manager uses it
void __osSiRelAccess_recomp(uint8_t*, recomp_context* ctx) { ret0(ctx); }
void __osPfsGetStatus_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 1; }   // PFS_ERR_NOPACK
}

// ---- PI: the game's own DMA thread (func_800BAE84) --------------------------------------------------------------
// It registers a queue for OS_EVENT_PI, starts each transfer with osPiRawStartDma and waits for that event. The
// runtime knows no OS_EVENT_PI and only has an aborting stub for osPiRawStartDma, so the port owns both (renamed in
// tools/recomp_syms.py, `ignored` in recomp/thps.toml): the transfer is done at once and the event posted.
#include <atomic>
#include <ultramodern/ultramodern.hpp>
#include "librecomp/game.hpp"

namespace {
std::atomic<int32_t> g_pi_mq{ 0 };
std::atomic<int32_t> g_pi_msg{ 0 };
constexpr int32_t kOsEventPi = 8;
}

extern "C" void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx);

extern "C" void thps_osSetEventMesg(uint8_t* rdram, recomp_context* ctx) {
    std::fprintf(stderr, "[thps] osSetEventMesg(%d, 0x%08X, 0x%X)\n", static_cast<int32_t>(ctx->r4),
                 static_cast<uint32_t>(ctx->r5), static_cast<uint32_t>(ctx->r6));
    if (static_cast<int32_t>(ctx->r4) == kOsEventPi) {
        g_pi_mq = static_cast<int32_t>(ctx->r5);
        g_pi_msg = static_cast<int32_t>(ctx->r6);
        return;
    }
    osSetEventMesg_recomp(rdram, ctx);
}

// s32 osPiRawStartDma(s32 direction, u32 devAddr, void* dramAddr, u32 size)
extern "C" void thps_osPiRawStartDma(uint8_t* rdram, recomp_context* ctx) {
    const int32_t direction = static_cast<int32_t>(ctx->r4);
    const uint32_t phys = (static_cast<uint32_t>(ctx->r5) | 0x10000000u) & 0x1FFFFFFFu;
    const gpr dram = ctx->r6;
    const uint32_t size = static_cast<uint32_t>(ctx->r7);
    static int traced = 0;
    if (traced++ < 400) {
        std::fprintf(stderr, "[thps] PI DMA %s ROM 0x%08X -> 0x%08X size 0x%X (pi mq 0x%08X)\n", direction ? "write" : "read",
                     phys, static_cast<uint32_t>(dram), size, static_cast<uint32_t>(g_pi_mq.load()));
    }
    if (direction == 0) {
        recomp::do_rom_read(rdram, dram, phys, size);
    } else {
        std::fprintf(stderr, "[thps] osPiRawStartDma write to 0x%08X ignored\n", phys);
    }
    if (g_pi_mq != 0) {
        ultramodern::enqueue_external_message_src(g_pi_mq, g_pi_msg, false, ultramodern::EventMessageSource::Pi);
    }
    ctx->r2 = 0;
}

// osRecvMesg with a trace of blocking receives (THPS_TRACE_RECV=1): which queue each thread waits on, to find stalls.
extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void thps_set_waiting(uint32_t mq);
extern "C" void thps_osRecvMesg(uint8_t* rdram, recomp_context* ctx) {
    thps_set_waiting(static_cast<uint32_t>(ctx->r4));
    static const bool trace = [] { const char* e = std::getenv("THPS_TRACE_RECV"); return e && *e == '1'; }();
    if (trace && static_cast<int32_t>(ctx->r6) == 1) {
        const uint32_t mq = static_cast<uint32_t>(ctx->r4);
        const int32_t count = MEM_W(0x8, static_cast<int32_t>(mq));   // OSMesgQueue.validCount
        if (count == 0) {
            std::fprintf(stderr, "[thps] recv blocks: mq 0x%08X ra 0x%08X\n", mq, static_cast<uint32_t>(ctx->r31));
        }
    }
    osRecvMesg_recomp(rdram, ctx);
    thps_set_waiting(0);
}

// ---- trace_mode: last recompiled function entered, per thread (recomp/include/trace.h) ---------------------------
#include <chrono>
#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif
#include "thps/crash_handler.h"
#include <mutex>
#include <thread>
namespace {
struct TraceSlot {
    std::atomic<const char*> last{ nullptr }; std::atomic<uint64_t> calls{ 0 }; std::atomic<uint32_t> waiting_mq{ 0 };
    std::atomic<const char*> ring[8]{};
    void* handle = nullptr;
};
TraceSlot g_slots[64];
std::atomic<int> g_nslots{ 0 };
thread_local TraceSlot* t_slot = nullptr;

void watchdog() {
    uint64_t prev[64] = {};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const int n = g_nslots.load();
        for (int i = 0; i < n; i++) {
            const uint64_t c = g_slots[i].calls.load();
            const char* last = g_slots[i].last.load();
            std::fprintf(stderr, "[thps] watch t%d: %-28s +%llu calls  waiting on mq 0x%08X\n", i, last ? last : "-",
                         static_cast<unsigned long long>(c - prev[i]), g_slots[i].waiting_mq.load());
            if (c == prev[i] && c > 0 && g_slots[i].waiting_mq.load() == 0 && g_slots[i].handle != nullptr
                && std::getenv("THPS_SAMPLE") != nullptr) {
                char what[32];
                std::snprintf(what, sizeof(what), "thread t%d", i);
                thps::sample_thread(g_slots[i].handle, what);
            }
            if (const uint32_t wq = g_slots[i].waiting_mq.load() & ~1u; wq != 0 && c == prev[i] && thps::g_rdram != nullptr) {
                uint8_t* rdram = thps::g_rdram;   // MEM_W needs it by this name
                std::fprintf(stderr, "[thps]   mq 0x%08X: mtqueue 0x%08X fullqueue 0x%08X validCount %d first %d msgCount %d\n",
                             wq, static_cast<uint32_t>(MEM_W(0x0, static_cast<int32_t>(wq))),
                             static_cast<uint32_t>(MEM_W(0x4, static_cast<int32_t>(wq))), MEM_W(0x8, static_cast<int32_t>(wq)),
                             MEM_W(0xC, static_cast<int32_t>(wq)), MEM_W(0x10, static_cast<int32_t>(wq)));
            }
            if (c == prev[i] && c > 0) {   // stalled: show how it got there (oldest first)
                std::fprintf(stderr, "[thps]   t%d recent:", i);
                for (uint64_t j = c >= 8 ? c - 8 : 0; j < c; j++) {
                    const char* r = g_slots[i].ring[j & 7].load();
                    std::fprintf(stderr, " %s", r ? r : "-");
                }
                std::fprintf(stderr, "\n");
            }
            prev[i] = c;
        }
    }
}
}  // namespace

namespace {
// THPS_PROFILE=1: calls per function (all threads), top 20 printed every 5 s and reset.
std::mutex g_prof_lock;
std::unordered_map<const char*, uint64_t> g_prof;

void profiler() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::vector<std::pair<const char*, uint64_t>> v;
        {
            std::lock_guard lock{ g_prof_lock };
            v.assign(g_prof.begin(), g_prof.end());
            g_prof.clear();
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        static int n_dump = 0;
        if (const char* dir = std::getenv("THPS_PROFILE_DUMP")) {   // full counts per window, one file each
            char path[512];
            std::snprintf(path, sizeof(path), "%s/profile_%03d.txt", dir, n_dump++);
            if (FILE* f = std::fopen(path, "w")) {
                for (const auto& [name, count] : v) std::fprintf(f, "%s %llu\n", name, static_cast<unsigned long long>(count));
                std::fclose(f);
            }
        }
        std::fprintf(stderr, "[thps] profile (5 s):");
        for (size_t i = 0; i < v.size() && i < 20; i++) std::fprintf(stderr, " %s:%llu", v[i].first, static_cast<unsigned long long>(v[i].second));
        std::fprintf(stderr, "\n");
    }
}
}  // namespace

extern "C" void thps_trace(const char* name, void* vctx) {
    static const bool profile = [] {
        const char* e = std::getenv("THPS_PROFILE");
        if (e && *e == '1') { std::thread(profiler).detach(); return true; }
        return false;
    }();
    if (profile) {
        std::lock_guard lock{ g_prof_lock };
        g_prof[name]++;
    }
    if (t_slot == nullptr) {
        const int i = g_nslots.fetch_add(1);
        t_slot = &g_slots[i < 64 ? i : 63];
#if defined(_WIN32)
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        t_slot->handle = h;
#endif
        static std::once_flag once;
        std::call_once(once, [] {
            const char* e = std::getenv("THPS_WATCH");
            if (e && *e == '1') std::thread(watchdog).detach();
        });
    }
    t_slot->last.store(name, std::memory_order_relaxed);
    // THPS_COUNT_FN=func_800A,func_800B: calls per second of every function whose name starts with one of the
    // prefixes, top 20 printed every 5 s (profiling which code runs how often).
    static const char* count_spec = std::getenv("THPS_COUNT_FN");
    if (count_spec != nullptr) {
        static const std::vector<std::string> prefixes = [] {
            std::vector<std::string> v;
            std::string sp = count_spec;
            size_t p0 = 0;
            while (p0 <= sp.size()) {
                size_t c = sp.find(',', p0);
                v.push_back(sp.substr(p0, c == std::string::npos ? std::string::npos : c - p0));
                if (c == std::string::npos) break;
                p0 = c + 1;
            }
            return v;
        }();
        bool hit = false;
        for (const std::string& pre : prefixes) hit = hit || std::strncmp(name, pre.c_str(), pre.size()) == 0;
        if (hit) {
            static std::mutex m;
            static std::unordered_map<const char*, uint64_t> counts;
            static auto last = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> g(m);
            counts[name]++;
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            static const double every = std::getenv("THPS_COUNT_SECS") ? std::atof(std::getenv("THPS_COUNT_SECS")) : 5.0;
            if (dt >= every) {
                std::vector<std::pair<uint64_t, const char*>> v;
                for (auto& [n, c] : counts) v.push_back({c, n});
                std::sort(v.rbegin(), v.rend());
                std::fprintf(stderr, "[thps-count] %.1f s:", dt);
                static const size_t top = std::getenv("THPS_COUNT_TOP") ? size_t(std::atoi(std::getenv("THPS_COUNT_TOP"))) : 20;
                for (size_t i = 0; i < v.size() && i < top; i++) std::fprintf(stderr, " %s=%.0f", v[i].second, double(v[i].first));
                std::fprintf(stderr, "\n");
                counts.clear();
                last = now;
            }
        }
    }
    // THPS_TRACE_FN=<name>: log the first calls' arguments of one function
    static const char* watch_fn = std::getenv("THPS_TRACE_FN");
    static const std::string watch_list = watch_fn ? std::string(",") + watch_fn + "," : std::string();
    if (watch_fn != nullptr && *watch_fn && watch_list.find(std::string(",") + name + ",") != std::string::npos) {
        static std::atomic<int> logged{ 0 };
        if (logged.fetch_add(1) < 5000) {
            const recomp_context* c = static_cast<const recomp_context*>(vctx);
            std::fprintf(stderr, "[thps] %s(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X) t%d\n", name,
                         static_cast<uint32_t>(c->r4), static_cast<uint32_t>(c->r5), static_cast<uint32_t>(c->r6),
                         static_cast<uint32_t>(c->r7), static_cast<int>(t_slot - g_slots));
        }
    }
    const uint64_t k = t_slot->calls.fetch_add(1, std::memory_order_relaxed);
    t_slot->ring[k & 7].store(name, std::memory_order_relaxed);
}

extern "C" void thps_set_waiting(uint32_t mq) {
    if (t_slot != nullptr) t_slot->waiting_mq.store(mq, std::memory_order_relaxed);
}

// osSendMesg, recording a blocking send (bit 0 set in the watchdog's queue address) so lock waits show up too.
extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void thps_osSendMesg(uint8_t* rdram, recomp_context* ctx) {
    thps_set_waiting(static_cast<uint32_t>(ctx->r4) | 1u);
    osSendMesg_recomp(rdram, ctx);
    thps_set_waiting(0);
}

// ---- AI: osAiGetStatus with a real FIFO-full bit ----------------------------------------------------------------
// The runtime's osAiGetStatus always says idle, so THPS (which loops `while (osAiGetStatus() < 0)` before queueing
// the next buffer) never throttles and builds up seconds of audio latency. Report AI_STATUS_FIFO_FULL while more
// than about two buffers are queued, sleeping 1 ms each time so the device drains in real time.
#include "thps/callbacks.h"
extern "C" void thps_osAiGetStatus(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    constexpr size_t kFullFrames = 3000;   // ~2 of THPS's ~1400-frame buffers at 22050 Hz
    if (thps::audio_frames_queued() > kFullFrames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ctx->r2 = static_cast<gpr>(static_cast<int32_t>(0xC0000000u));   // FIFO full | busy
        return;
    }
    ctx->r2 = 0;
}
