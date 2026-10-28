// thpSkate3 bridge: skatecore (the Skate 3 runtime) skates in place of THPS's skater physics.
//
// Two hooks are injected into recompiled THPS code (recomp/thps.toml, recomp/include/skate_hooks.h):
//  - func_80059BEC, the skater's physics/state machine, called once per THPS frame from the skater update
//    func_8004BE0C. With skating on, THPS's version never runs: skatecore steps on the raw Xbox pad and the
//    result is written into THPS's skater object, so everything else THPS does with the skater (pickups, goals,
//    the HUD, triggers) sees the Skate 3 skater.
//  - func_80073EA0, which turns THPS's camera object into the view record D_800DE1A4[player]. With skating on,
//    the Skate 3 camera is written into the camera object first and THPS's function then runs as usual, so the
//    view record, the GTE and the level's visibility culling all agree.
//
// Frames and units: THPS world units with PSX axes (y down); skatecore Y-up metres. See src/skate/level.hpp.
// Skater object (include/types/object.h in the THPS64 decomp): +0x4 position (Vec3i, units << 12), +0x82C
// physics state (1 ground, 2 air), +0x884 orientation (s16 3x3, x.12, columns = side, up, forward in PSX
// axes), +0x8D0 forward (Vec3i x.12). Camera object: +0x4 eye (units << 12) plus an offset at +0x6F0, +0x500
// look-at target, +0x554 rotation quaternion (x, y, z, w in x.12; func_80067A48 makes it the view matrix,
// whose columns are right, down, forward).
//
// Environment: THPS_SKATE=0 leaves THPS's own skater; THPS_SKATE_CAM=0 keeps THPS's camera; THPS_SKATE_PACK /
// THPS_SKATE_LEVELS override where the cleanroom core.pack and the level caches (tools/thps_level.py) are;
// THPS_SKATE_SCALE sets THPS units per metre.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <cctype>
#include <sstream>
#include <map>
#include <fstream>
#include <iterator>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "recomp.h"

#include "s3/data/animation_banks.hpp"
#include "s3/data/vfs.hpp"
#include "s3/game/driver.hpp"
#include "skate/level.hpp"
#include "thps/skate_trainer.h"
#include "thps/xpad.h"

extern "C" int thps_skate_physics(uint8_t* rdram, recomp_context* ctx);
extern "C" int thps_skate_camera(uint8_t* rdram, recomp_context* ctx);
extern "C" void thps_skate_pose_begin(uint8_t* rdram, recomp_context* ctx);
extern "C" void thps_skate_pose_end(uint8_t* rdram, recomp_context* ctx);

namespace {

using s3::game::driver::Pad;
using s3::game::driver::Sim;
using Vec3 = std::array<float, 3>;

// ------------------------------------------------------------------ RDRAM ----
// The recompiler keeps RDRAM as host-order 32-bit words: words are read directly, halfwords at addr ^ 2,
// bytes at addr ^ 3.
uint32_t r32(const uint8_t* rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a - 0x80000000u), 4); return v; }
int32_t s32(const uint8_t* rdram, uint32_t a) { return int32_t(r32(rdram, a)); }
void w32(uint8_t* rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a - 0x80000000u), &v, 4); }
void w16(uint8_t* rdram, uint32_t a, int16_t v) { std::memcpy(rdram + ((a ^ 2) - 0x80000000u), &v, 2); }
uint8_t r8(const uint8_t* rdram, uint32_t a) { return rdram[(a ^ 3) - 0x80000000u]; }
std::string rstr(const uint8_t* rdram, uint32_t a, size_t max = 64) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        const char c = char(r8(rdram, a + uint32_t(i)));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}
bool ram_ptr(uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; }

constexpr uint32_t kLevelIndex = 0x800C0E20;   // index into D_800D5C1C (0x78-byte level records)
constexpr uint32_t kLevelTable = 0x800D5C1C;   // +0x14: trigger file stem, e.g. "SkWare_T"

// ------------------------------------------------------------------ maths ----
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 norm(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-9f ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 1};
}
// A direction between the two frames (the same rotation as positions, without the scale); it is its own inverse.
Vec3 flip_yz(const Vec3& v) { return {v[0], -v[1], -v[2]}; }
Vec3 quat_rotate(const std::array<float, 4>& q, const Vec3& v) {  // q = x, y, z, w
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 t = cross(u, v);
    const Vec3 t2{2 * t[0], 2 * t[1], 2 * t[2]};
    const Vec3 c = cross(u, t2);
    return {v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
}
int16_t fx12(float v) {
    const float s = std::round(v * 4096.0f);
    return int16_t(s > 32767.0f ? 32767 : (s < -32768.0f ? -32768 : s));
}
// Quaternion (x, y, z, w) whose standard rotation matrix has these rows. THPS's func_80067A48 builds the
// transpose of that matrix, i.e. one whose columns are r0, r1, r2.
std::array<float, 4> quat_from_rows(const Vec3& r0, const Vec3& r1, const Vec3& r2) {
    const float m00 = r0[0], m01 = r0[1], m02 = r0[2], m10 = r1[0], m11 = r1[1], m12 = r1[2];
    const float m20 = r2[0], m21 = r2[1], m22 = r2[2];
    const float tr = m00 + m11 + m22;
    float x, y, z, w;
    if (tr > 0) {
        const float S = std::sqrt(tr + 1.0f) * 2;
        w = 0.25f * S; x = (m21 - m12) / S; y = (m02 - m20) / S; z = (m10 - m01) / S;
    } else if (m00 > m11 && m00 > m22) {
        const float S = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        w = (m21 - m12) / S; x = 0.25f * S; y = (m01 + m10) / S; z = (m02 + m20) / S;
    } else if (m11 > m22) {
        const float S = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        w = (m02 - m20) / S; x = (m01 + m10) / S; y = 0.25f * S; z = (m12 + m21) / S;
    } else {
        const float S = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        w = (m10 - m01) / S; x = (m02 + m20) / S; y = (m12 + m21) / S; z = 0.25f * S;
    }
    return {x, y, z, w};
}

// ------------------------------------------------------------------ state ----
struct Bridge {
    bool enabled = true;
    bool camera = true;
    bool failed = false;                       // could not load: THPS keeps its own skater
    float units = thpskate::kUnitsPerMetre;
    float pending_units = 0.0f;                // skater size being dragged in the trainer
    int pending_frames = 0;
    bool units_from_env = false;               // THPS_SKATE_SCALE (else the trainer's skater size)
    std::filesystem::path pack_path, levels_dir;
    std::unique_ptr<s3::data::Vfs> vfs;
    std::unique_ptr<Sim> sim;
    std::string level;                         // loaded level ("SkWare")
    uint32_t skater = 0;                       // THPS skater object the sim drives
    std::array<int32_t, 3> written{};          // last position written (units << 12)
    bool have_written = false;
    std::array<int32_t, 3> prev_written{};     // the frame before (for the velocity THPS's sounds read)
    bool have_prev_written = false;
    s3::game::driver::Presentation last;
    bool have_presentation = false;
    struct Trigger {
        std::array<Vec3, 3> v;  // THPS units
        uint32_t key = 0;       // TRG node key of the triangle's object
    };
    std::vector<Trigger> triggers;
    Vec3 prev_origin{};         // THPS skater origin last frame (units)
    bool have_prev = false;
    // Retargeting (see the pose section at the end): per THPS bone, the Skate 3 bone that drives it and the
    // constant offset between them, measured once per spawn.
    bool pose_calibrated = false;
    std::vector<int> pose_source;
    std::vector<std::array<double, 16>> pose_offset;
    // Limb pieces: anchored by their vertex centroid (pose_centroid, THPS bone space) onto a point fixed in the
    // Skate 3 bone (pose_anchor, metres): the middle of the bone segment, or just past the joint for hands and feet.
    std::vector<bool> pose_limb;
    std::vector<std::array<double, 3>> pose_centroid, pose_anchor;
    double pose_scale = 1.0;                   // character height / Skate 3 rig height, about the root
    struct Joint { int parent, child; std::array<double, 3> parent_local, child_local; };
    std::vector<Joint> pose_joints;            // THPS's own joints, parents before children
    thpskate::LevelGeometry geo;               // the loaded level's cache (tools/thps_level.py)
    // Trainer (src/skate_trainer.cpp)
    bool tuning_applied = false;
    thps::trainer::Settings applied{};
    float tick_accumulator = 0.0f;
    struct Marker { bool set = false; Vec3 pos{}; float heading = 0.0f; std::string level; };
    std::array<Marker, thps::trainer::kMarkers> markers{};
    std::string last_trick;
    bool was_pushing = false;
    uint16_t session_buttons = 0;              // pad buttons last frame (session marker edges)
    int bail_timer = 0;                        // frames until auto-return after a bail
    std::array<float, 4> last_deck{0, 0, 0, 1};
    std::string geo_level;
    // A world being rebuilt in the background (skater size, session marker): see spawn().
    std::future<s3::rs::Result<std::unique_ptr<Sim>, std::string>> build;
    std::string build_level;
    uint32_t build_skater = 0;
    float build_units = 0.0f;
};

Bridge& bridge() {
    static Bridge b = [] {
        Bridge b;
        if (const char* e = std::getenv("THPS_SKATE"); e != nullptr && *e == '0') b.enabled = false;
        if (const char* e = std::getenv("THPS_SKATE_CAM"); e != nullptr && *e == '0') b.camera = false;
        if (const char* e = std::getenv("THPS_SKATE_SCALE")) {
            b.units = std::strtof(e, nullptr);
            b.units_from_env = true;
        } else {
            b.units = thpskate::kUnitsPerMetre * thps::trainer::get().skater_size;
        }
        // The executable is <repo>/port/build/THPS64Recomp.
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        const auto root = ec ? std::filesystem::current_path() : exe.parent_path().parent_path().parent_path();
        b.pack_path = std::getenv("THPS_SKATE_PACK") ? std::filesystem::path(std::getenv("THPS_SKATE_PACK"))
                                                     : root / "data" / "cleanpack" / "core.pack";
        b.levels_dir = std::getenv("THPS_SKATE_LEVELS") ? std::filesystem::path(std::getenv("THPS_SKATE_LEVELS"))
                                                        : root / "work" / "levels";
        return b;
    }();
    return b;
}

bool load_pack(Bridge& b) {
    if (b.vfs) return true;
    std::ifstream f(b.pack_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[skate] no core.pack at %s: THPS keeps its own skater (set THPS_SKATE_PACK)\n",
                     b.pack_path.c_str());
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto vfs = s3::data::Vfs::from_pack(std::move(bytes));
    if (!vfs.ok) {
        std::fprintf(stderr, "[skate] core.pack: %s\n", vfs.error.c_str());
        return false;
    }
    b.vfs = std::make_unique<s3::data::Vfs>(std::move(vfs.value));
    std::fprintf(stderr, "[skate] core.pack loaded (%s)\n", b.pack_path.c_str());
    return true;
}

std::string current_level(const uint8_t* rdram) {
    const int32_t idx = s32(rdram, kLevelIndex);
    if (idx < 0 || idx > 16) return {};
    const uint32_t name = r32(rdram, kLevelTable + uint32_t(idx) * 0x78 + 0x14);
    if (!ram_ptr(name)) return {};
    std::string s = rstr(rdram, name);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_T") == 0) s.resize(s.size() - 2);
    return s;
}

Vec3 skater_pos_units(const uint8_t* rdram, uint32_t sk) {
    return {s32(rdram, sk + 4) / 4096.0f, s32(rdram, sk + 8) / 4096.0f, s32(rdram, sk + 0xC) / 4096.0f};
}

// Facing of the THPS skater as a Skate 3 heading about +Y. THPS stores the facing reversed: column 2 of the
// +0x884 matrix (and +0x8D0) points backwards (seen in the Warehouse drop-in: the skater rolls toward +z while
// both say -z). skatecore's heading 0 rolls toward +Z (measured with tools/native/skate_on_thps).
float skater_heading(const uint8_t* rdram, uint32_t sk) {
    auto m = [&](int r, int c) {
        const uint32_t a = sk + 0x884 + 2 * uint32_t(3 * r + c);
        int16_t v;
        std::memcpy(&v, rdram + ((a ^ 2) - 0x80000000u), 2);
        return v / 4096.0f;
    };
    const Vec3 f = flip_yz({-m(0, 2), -m(1, 2), -m(2, 2)});
    return std::atan2(f[0], f[2]);
}

// THPS's skater origin sits about 30 units over the ground; skatecore's spawn is the wheel-ground anchor.
constexpr float kOriginHeight = 30.0f;

const thpskate::LevelGeometry* level_geometry(Bridge& b, const std::string& level) {
    if (b.geo_level == level) return &b.geo;
    std::string err;
    const auto path = b.levels_dir / (level + ".thl");
    b.geo = {};
    b.geo_level.clear();
    if (!thpskate::load_thl(path.string(), b.geo, err)) {
        std::fprintf(stderr, "[skate] %s (export the levels with tools/thps_level.py)\n", err.c_str());
        return nullptr;
    }
    b.geo_level = level;
    return &b.geo;
}

// The restart point THPS put the skater on, if any: its facing as a Skate 3 heading. A restart's angle a (4096ths
// of a turn) faces THPS travel (-sin a, 0, -cos a) (the Warehouse start, a = 0x801, rolls toward +z), which is
// heading -a in skatecore's frame.
bool restart_heading(Bridge& b, const std::string& level, const Vec3& pos, float& heading) {
    const auto* geo = level_geometry(b, level);
    if (!geo) return false;
    for (const auto& r : geo->restarts) {
        const float dx = r.pos[0] - pos[0], dy = r.pos[1] - pos[1], dz = r.pos[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz < 96.0f * 96.0f) {
            heading = -float(r.angle) * 6.2831853f / 4096.0f;
            return true;
        }
    }
    return false;
}

using SimLoad = s3::rs::Result<std::unique_ptr<Sim>, std::string>;

// Second half of a spawn: take the built sim and reset everything tied to the old one.
bool finish_spawn(Bridge& b, SimLoad loaded, const std::string& level, uint32_t sk, float units) {
    const auto* geop = level_geometry(b, level);
    if (!geop) return false;
    const thpskate::LevelGeometry& geo = *geop;
    if (loaded.is_err()) {
        std::fprintf(stderr, "[skate] Sim::load: %s\n", loaded.unwrap_err().c_str());
        return false;
    }
    b.sim = std::move(loaded.unwrap());
    b.units = units;
    b.triggers.clear();
    for (const auto& t : geo.tris) {
        if (!t.trigger() || t.object >= geo.object_keys.size() || geo.object_keys[t.object] == 0) continue;
        b.triggers.push_back({{t.v[0], t.v[1], t.v[2]}, geo.object_keys[t.object]});
    }
    b.have_prev = false;
    b.level = level;
    b.skater = sk;
    b.have_written = false;
    b.have_prev_written = false;
    b.have_presentation = false;
    b.pose_calibrated = false;
    b.tuning_applied = false;
    return true;
}

// (Re)build the Skate 3 world around the THPS skater's position. Building takes about half a second
// (Sim::load), so `background` does it on a worker thread: the hook keeps the skater still and the game running
// until it is ready (see b.build in thps_skate_physics). units: the skater size to build at (default: current).
bool spawn(Bridge& b, uint8_t* rdram, uint32_t sk, const std::string& level, const float* heading,
           bool background = false, float units = 0.0f) {
    const auto* geop = level_geometry(b, level);
    if (!geop) return false;
    const thpskate::LevelGeometry& geo = *geop;
    if (units <= 0.0f) units = b.units;
    thpskate::MapOptions opt;
    opt.units_per_metre = units;
    if (const char* e = std::getenv("THPS_SKATE_EDGES")) opt.edge_codes = std::atoi(e) != 0;  // 0: skatecore's own edge pairing
    Vec3 p = skater_pos_units(rdram, sk);
    p[1] += kOriginHeight;
    opt.spawn = thpskate::to_s3(p, units);
    opt.heading = heading ? *heading : skater_heading(rdram, sk);
    auto map = thpskate::make_map(geo, level, opt);
    std::fprintf(stderr, "[skate] %s: %zu triangles, spawned at (%.0f %.0f %.0f) units heading %.2f\n",
                 level.c_str(), map.geometry.collision.size(), p[0], p[1], p[2], opt.heading);
    if (background) {
        b.build = std::async(std::launch::async, [vfs = b.vfs.get(), map = std::move(map)]() {
            return Sim::load(*vfs, "assets", &map, nullptr);
        });
        b.build_level = level;
        b.build_skater = sk;
        b.build_units = units;
        return true;
    }
    return finish_spawn(b, Sim::load(*b.vfs, "assets", &map, nullptr), level, sk, units);
}

Pad pad_from_xpad(int player) {
    const thps::xpad::State x = thps::xpad::get(player);
    Pad p;
    p.buttons = x.buttons;
    p.left_trigger = x.left_trigger;
    p.right_trigger = x.right_trigger;
    p.lx = x.lx;
    p.ly = x.ly;
    p.rx = x.rx;
    p.ry = x.ry;
    return p;
}

// THPS's physics state (+0x82C, set through func_8004D048): 0 on the ground, 1 in the air, 2 vert air, 4 grinding,
// 5 wallriding, 7 bailing. Gap scripts, the HUD and the sound loops read it.
int thps_state(const s3::game::driver::TickRecord& r) {
    const std::string& s = r.physical_state;
    if (r.wiping_out || s == "WipeoutGround" || s.rfind("Biped", 0) == 0 || s == "Sleeping" || s == "Teleporting")
        return 7;
    if (s.rfind("Grind", 0) == 0) return 4;
    if (!r.grounded || s == "PhysicsAir" || s == "KnownAir" || s == "PhysicsAirSecondary" || s == "Boneless")
        return 1;
    return 0;
}

void write_skater(Bridge& b, uint8_t* rdram, uint32_t sk, const s3::game::driver::TickRecord& r) {
    const auto& pres = b.last;
    // Root of the animated skater (at the feet), skatecore metres -> THPS units.
    Vec3 root = thpskate::from_s3({pres.root[3][0], pres.root[3][1], pres.root[3][2]}, b.units);
    root[1] -= kOriginHeight;
    // +0x16C is last frame's position, kept by the physics routine this replaces (func_80059BEC: V3(0x16C) =
    // V3(0x4) before it moves the skater; its V3(0x2A8) copy is left alone, writing it stopped the skater). After
    // the skater update THPS collects pickups (letters, tapes) along the segment +0x16C -> +0x4 (func_8004BE0C), so
    // a stale +0x16C collected every item between where it was last set and the skater. A teleport (respawn,
    // marker, Skate 3's own reset) starts the segment afresh.
    std::array<int32_t, 3> pos{};
    double moved2 = 0.0;
    for (int i = 0; i < 3; i++) {
        pos[size_t(i)] = int32_t(std::lround(double(root[size_t(i)]) * 4096.0));
        const double d = (double(pos[size_t(i)]) - double(s32(rdram, sk + 4 + 4 * uint32_t(i)))) / 4096.0;
        moved2 += d * d;
    }
    const bool fresh = !b.have_written || moved2 > double(2.5f * b.units) * double(2.5f * b.units);
    for (int i = 0; i < 3; i++) {
        const uint32_t k = 4 * uint32_t(i);
        w32(rdram, sk + 0x16C + k, fresh ? uint32_t(pos[size_t(i)]) : r32(rdram, sk + 4 + k));
        w32(rdram, sk + 4 + k, uint32_t(pos[size_t(i)]));
        b.written[size_t(i)] = pos[size_t(i)];
    }
    b.have_written = true;
    // Orientation from the deck, in THPS's own (mirrored) convention: column 1 up, column 2 = -forward,
    // column 0 = up x forward; +0x8D0 is -forward too.
    const auto& deck = r.board.back();
    const Vec3 fwd = norm(flip_yz(quat_rotate(deck.orientation, {0, 0, 1})));
    const Vec3 up = norm(flip_yz(quat_rotate(deck.orientation, {0, 1, 0})));
    const Vec3 back{-fwd[0], -fwd[1], -fwd[2]};
    const Vec3 cols[3] = {norm(cross(up, fwd)), up, back};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) w16(rdram, sk + 0x884 + 2 * uint32_t(3 * i + j), fx12(cols[j][size_t(i)]));
    for (int i = 0; i < 3; i++)
        w32(rdram, sk + 0x8D0 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(back[size_t(i)] * 4096.0f))));
    // Velocity (+0x84, units << 12 per 30 Hz frame): THPS's sound loops take their volume and pitch from it.
    if (b.have_prev_written)
        for (int i = 0; i < 3; i++)
            w32(rdram, sk + 0x84 + 4 * uint32_t(i), uint32_t(b.written[size_t(i)] - b.prev_written[size_t(i)]));
    b.prev_written = b.written;
    b.have_prev_written = true;
    w32(rdram, sk + 0x82C, uint32_t(thps_state(r)));
}

// --------------------------------------------------------------- triggers ----
// THPS fires a level object's TRG script when the skater's movement segment crosses one of its trigger faces
// (face word bit 0x20000; func_800837C8 -> func_80061D7C(key, 1), which runs each node once per pass). That is
// how glass breaks and gaps start and end. The same test runs here on the Skate 3 skater's motion.
extern "C" void func_80061D7C(uint8_t* rdram, recomp_context* ctx);
// func_80059BEC brackets its trigger test the same way: func_80061E7C(player) opens a pass, D_800D61A0 holds the
// skater the scripts act on (gaps read its pending-gap lists), func_80061E34(player) closes the pass and re-arms
// the triggers that were not touched in it.
extern "C" void func_80061E7C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80061E34(uint8_t* rdram, recomp_context* ctx);
constexpr uint32_t kTriggerSkater = 0x800D61A0;
constexpr uint32_t kCurrentSkater = 0x800DE7A8;

// Segment p->q against a triangle, either side (Moller-Trumbore).
bool segment_hits(const Vec3& p, const Vec3& q, const std::array<Vec3, 3>& t) {
    const Vec3 d{q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const Vec3 e1{t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]};
    const Vec3 e2{t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
    const Vec3 h = cross(d, e2);
    const float a = dot(e1, h);
    if (std::fabs(a) < 1e-6f) return false;
    const float f = 1.0f / a;
    const Vec3 s{p[0] - t[0][0], p[1] - t[0][1], p[2] - t[0][2]};
    const float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 qv = cross(s, e1);
    const float v = f * dot(d, qv);
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = f * dot(e2, qv);
    return tt >= 0.0f && tt <= 1.0f;
}

void run_triggers(Bridge& b, uint8_t* rdram, const recomp_context* ctx, const Vec3& origin);

// ---------------------------------------------------------------- scoring ----
// Skate 3 tricks go through THPS's own combo system (THPS64 decomp):
//   func_8004C9E8(skater)                     start a combo (sets +0x858, clears the per-combo counters)
//   func_8004CACC(skater, id, name, points)   add a trick: points > 0 decay with repeats (+0x520[id]),
//                                             points < 0 are fixed; name is a char* the HUD shows
//   func_8004CC10(skater)                     land: spin bonus from +0x424 (half turns), banks the combo
//   func_8004CED0(skater)                     bail: drops the combo
// They are recompiled game code, called here on the game thread from inside the hook.
extern "C" {
void func_8004C9E8(uint8_t* rdram, recomp_context* ctx);
void func_8004CACC(uint8_t* rdram, recomp_context* ctx);
void func_8004CC10(uint8_t* rdram, recomp_context* ctx);
void func_8004CED0(uint8_t* rdram, recomp_context* ctx);
}

void call_game(void (*fn)(uint8_t*, recomp_context*), uint8_t* rdram, const recomp_context* at, uint32_t a0,
               uint32_t a1 = 0, uint32_t a2 = 0, int32_t a3 = 0) {
    recomp_context c = *at;
    c.r4 = gpr(int64_t(int32_t(a0)));
    c.r5 = gpr(int64_t(int32_t(a1)));
    c.r6 = gpr(int64_t(int32_t(a2)));
    c.r7 = gpr(int64_t(a3));
    c.r29 = at->r29 - 0x400;  // below the hooked function's frame
    w32(rdram, uint32_t(c.r29) + 0x10, 0);  // fifth argument slot
    fn(rdram, &c);
}

// Trick names live above the 8 MB the game uses (the recompiler's RDRAM is larger), one C string each.
constexpr uint32_t kStringPool = 0x80F00000u;
uint32_t game_string(uint8_t* rdram, const std::string& text) {
    static std::vector<std::pair<std::string, uint32_t>> pool;
    static uint32_t next = kStringPool;
    for (const auto& [t, a] : pool) if (t == text) return a;
    const uint32_t a = next;
    for (size_t i = 0; i <= text.size(); i++) rdram[((a + uint32_t(i)) ^ 3) - 0x80000000u] = i < text.size() ? uint8_t(text[i]) : 0;
    next += uint32_t((text.size() + 4) & ~3u);
    pool.push_back({text, a});
    return a;
}


// THPS-scale points for a Skate 3 trick (THPS: kickflip 100, hardflip 300, grabs 200-ish, grinds 150).
int trick_points(const std::string& id) {
    auto has = [&](const char* w) { return id.find(w) != std::string::npos; };
    int p = 100;
    if (id == "OLLIE" || id == "NOLLIE" || id == "N_OLLIE") p = 50;
    if (has("VARIAL")) p += 100;
    if (has("HARDFLIP") || has("INWARD")) p += 200;
    if (has("360") || has("TRE")) p += 300;
    if (has("LASER") || has("IMPOSSIBLE")) p += 300;
    if (has("UNDERFLIP") || has("FINGERFLIP") || has("DARKSLIDE") || has("CASPER")) p += 150;
    if (has("GRAB") || has("INDY") || has("MELON") || has("METHOD") || has("STALE") || has("MUTE") ||
        has("JAPAN") || has("AIRWALK")) p += 100;
    if (has("GRIND") || has("SLIDE") || has("50") || has("SMITH") || has("FEEBLE") || has("CROOK")) p += 50;
    if (has("DOUBLE")) p *= 2;
    if (has("TRIPLE")) p *= 3;
    if (id.rfind("N_", 0) == 0 || id.rfind("F_", 0) == 0 || id.rfind("S_", 0) == 0) p += 50;
    return p;
}

// A stable THPS trick slot per Skate 3 trick, so THPS's per-trick repeat decay applies (ids 0x10-0x3F; the
// game's own special ids 0x40-0x4F stay free).
uint32_t trick_slot(const std::string& id) {
    uint32_t h = 2166136261u;
    for (char c : id) h = (h ^ uint8_t(c)) * 16777619u;
    return 0x10 + h % 0x30;
}

void run_triggers(Bridge& b, uint8_t* rdram, const recomp_context* ctx, const Vec3& origin) {
    static const bool log = std::getenv("THPS_SKATE_LOG") != nullptr;
    const uint32_t who = b.skater == r32(rdram, kCurrentSkater) ? 1u : 2u;
    call_game(func_80061E7C, rdram, ctx, who);
    w32(rdram, kTriggerSkater, b.skater);
    if (b.have_prev) {
        // One segment, as func_80059BEC builds it: from last frame's origin (pulled 16 units further back along
        // the motion) to this frame's. Testing higher up as well collected pickups floating over the skater.
        Vec3 p = b.prev_origin;
        const Vec3 d{origin[0] - p[0], origin[1] - p[1], origin[2] - p[2]};
        const float len = std::sqrt(dot(d, d));
        if (len > 1e-3f)
            for (int k = 0; k < 3; k++) p[size_t(k)] -= d[size_t(k)] / len * 16.0f;
        // Skate 3 moves the skater itself after a wipeout (its Teleporting reset), which does not go through
        // spawn(): a segment from the crash to the reset point would collect every letter and tape between them.
        // Real motion is at most ~0.5 m a THPS frame; anything over 2.5 m is a teleport and tests nothing.
        const bool teleport = len > 2.5f * b.units;
        if (teleport && log) std::fprintf(stderr, "[skate] teleport of %.0f units: no trigger test\n", len);
        uint32_t fired = 0;
        for (const auto& t : b.triggers) {
            if (teleport) break;
            if (t.key == fired || !segment_hits(p, origin, t.v)) continue;
            call_game(func_80061D7C, rdram, ctx, t.key, 1);
            fired = t.key;
            if (log) std::fprintf(stderr, "[skate] trigger %08X (score %d)\n", t.key, s32(rdram, b.skater + 0x2E8));
        }
    }
    call_game(func_80061E34, rdram, ctx, who);
    w32(rdram, kTriggerSkater, 0);
    b.prev_origin = origin;
    b.have_prev = true;
}

// ---------------------------------------------------------- THPS-style combos ----
// Skate 3 names what the skater is doing as it happens (TickRecord::score_name, e.g.
// "#ID_TRICK_GRAB_INDY 540 ID_TRICK_AIR_METRICS_BACKFLIP" or "#GRIND_FS_FEEBLE"). The bridge splits a run into
// segments -- an air, a grind, a manual, a hand or foot plant -- and adds each finished segment to THPS's combo
// (func_8004CACC) under its Skate 3 name, so THPS's own combo code counts the tricks, multiplies the total by
// that count and banks it. Like THPS, a combo carries on while one segment leads into the next (land into a
// manual, manual into an ollie, air into a grind...) and banks once the skater just rolls (after a short grace
// for Skate 3's landing frames); a bail loses it.
// English names come from the disc's language table (data/cleanpack/trick-names.tsv, written by
// tools/import_skate3.sh); without it the ids are tidied up instead.
struct ComboState {
    enum class Seg { None, Air, Grind, Manual, Plant };
    Seg seg = Seg::None;
    std::string name;            // the segment's Skate 3 score name
    int ticks = 0;               // ticks spent in the segment
    int rolling = 0;             // ticks of plain rolling since the last segment ended
    bool in_combo = false;
    std::string announced;       // the last score name Skate 3 showed (it stays up after a trick ends)
    std::string pending;         // a name announced for a grind/manual/plant just before its state starts
    int pending_age = 1000;
    int gap = 0;                 // ticks the segment's state has been missing (it flickers mid-grind)
    int entry = -1;              // this segment's slot in THPS's combo list, once added
    float ratio = 1.0f;          // THPS's stored points / ours (repeat decay, the x10 mode)
    std::string shown;           // name and points last written to the slot
    int shown_points = -1;
};

const std::map<std::string, std::string>& trick_names(const Bridge& b) {
    static std::map<std::string, std::string> names;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(b.pack_path.parent_path() / "trick-names.tsv");
        for (std::string line; std::getline(f, line);) {
            const size_t tab = line.find('\t');
            // Skate 3's table has a few unfinished entries ("FILL ME IN HAL"): leave those out.
            if (tab != std::string::npos && line.find("FILL ME IN", tab) == std::string::npos)
                names[line.substr(0, tab)] = line.substr(tab + 1);
        }
        std::fprintf(stderr, "[skate] %zu Skate 3 trick names\n", names.size());
    }
    return names;
}

// "#ID_TRICK_GRAB_INDY 540 ID_TRICK_AIR_METRICS_BACKFLIP" -> "INDY 540 BACKFLIP" (THPS's HUD font is upper case).
std::string skate3_name(const Bridge& b, const std::string& score_name) {
    const auto& names = trick_names(b);
    std::string out, token;
    std::istringstream in(score_name.size() && score_name[0] == '#' ? score_name.substr(1) : score_name);
    while (in >> token) {
        std::string text;
        if (auto it = names.find(token); it != names.end()) text = it->second;
        else if (auto it2 = names.find("ID_TRICK_" + token); it2 != names.end()) text = it2->second;
        else if (!token.empty() && !std::isdigit(uint8_t(token[0]))) {
            static std::set<std::string> reported;  // names the disc's table lacks: tidy the id, say so once
            if (reported.insert(token).second) std::fprintf(stderr, "[skate] no Skate 3 name for %s\n", token.c_str());
            std::string t = token.rfind("ID_TRICK_", 0) == 0 ? token.substr(9) : token;
            for (const char* pre : {"GRIND_", "GROUND_TRICK_", "FLIP_", "GRAB_", "AIR_METRICS_", "AIR_"})
                if (t.rfind(pre, 0) == 0) { t = t.substr(std::strlen(pre)); break; }
            for (size_t i = 0; i + 2 < t.size(); i++)  // 5_0 -> 5-0, 50_50 -> 50-50
                if (t[i + 1] == '_' && std::isdigit(uint8_t(t[i])) && std::isdigit(uint8_t(t[i + 2]))) t[i + 1] = '-';
            for (char& c : t) if (c == '_') c = ' ';
            text = t;
        } else {
            text = token;  // spin degrees
        }
        if (!out.empty()) out += ' ';
        out += text;
    }
    for (char& c : out) c = char(std::toupper(uint8_t(c)));
    return out;
}

// THPS-scale points for a finished segment (THPS: ollie 50, kickflip 100, hardflip 300, grinds ~150 plus time).
int segment_points(ComboState::Seg seg, const std::string& score_name, int ticks) {
    const float secs = float(ticks) / 60.0f;
    auto has = [&](const char* w) { return score_name.find(w) != std::string::npos; };
    int p = 0;
    switch (seg) {
    case ComboState::Seg::Grind: p = 150 + int(300.0f * secs); break;
    case ComboState::Seg::Manual: p = 100 + int(200.0f * secs); break;
    case ComboState::Seg::Plant: p = 250; break;
    default: {
        // the base trick, then the spin (each 180 degrees) and body flips on top
        std::string base = score_name.substr(0, score_name.find(' '));
        if (base.size() && base[0] == '#') base = base.substr(1);
        p = (base == "ID_TRICK_AIR" || has("FLIP_OLLIE") || has("FLIP_NOLLIE")) ? 50 : trick_points(base);
        if (has("GRAB")) p += 100 + int(100.0f * secs);
        std::istringstream in(score_name);
        for (std::string t; in >> t;)
            if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(uint8_t(c)); }))
                p += std::atoi(t.c_str()) / 180 * 100;
        if (has("FRONTFLIP") || has("BACKFLIP")) p += 500;
        if (has("DARKCATCH")) p += 200;
        break;
    }
    }
    return p;
}

// THPS's combo list (THPS64 decomp, func_800460B8 / func_80045CDC): at skater +0xA1C, entries of 20 bytes from
// +0x1F8 (name pointer, points, ...), count at +0x388. Banking sums the entries' points and multiplies by the
// count, so a slot can be updated in place while its trick goes on; func_80045A70 rebuilds the HUD text.
extern "C" void func_80045A70(uint8_t* rdram, recomp_context* ctx);
constexpr uint32_t kComboList = 0xA1C, kComboEntries = 0x1F8, kComboCount = 0x388;

// A trick id with the spelling differences between Skate 3's TRICK ids and its language table's keys taken out:
// category prefixes, NOLLIE vs N_, NOSEGRIND vs NOSE, 5_O vs 5_0 and the underscores
// (360FLIP ~ ID_TRICK_FLIP_360_FLIP, BS_5_O ~ GRIND_BS_5_0, N_POPSHUVIT ~ ID_TRICK_FLIP_NOLLIE_POP_SHUVIT).
std::string trick_key(std::string k) {
    for (const char* pre : {"ID_TRICK_", "#"})
        if (k.rfind(pre, 0) == 0) k = k.substr(std::strlen(pre));
    for (const char* pre : {"FLIP_", "GRIND_", "GRAB_", "GROUND_TRICK_", "FINGERFLIP_"})
        if (k.rfind(pre, 0) == 0) { k = k.substr(std::strlen(pre)); break; }
    if (k.rfind("NOLLIE_", 0) == 0) k = "N_" + k.substr(7);
    auto swap_all = [&](const std::string& from, const std::string& to) {
        for (size_t i = k.find(from); i != std::string::npos; i = k.find(from, i + to.size())) k.replace(i, from.size(), to);
    };
    swap_all("NOSEGRIND", "NOSE");
    if (k.size() >= 2 && k.compare(k.size() - 2, 2, "_O") == 0) k[k.size() - 1] = '0';
    swap_all("_O_", "_0_");
    k.erase(std::remove(k.begin(), k.end(), '_'), k.end());
    return k;
}

// A TRICK event's id (OLLIE, HEELFLIP, FS_50_50, 360FLIP) as a score name the table knows, or a tidied id.
std::string trick_score_name(const Bridge& b, const std::string& id) {
    const auto& names = trick_names(b);
    for (const char* pre : {"ID_TRICK_FLIP_", "GRIND_", "ID_TRICK_GRAB_", "ID_TRICK_GROUND_TRICK_", "ID_TRICK_"})
        if (names.count(pre + id)) return std::string("#") + pre + id;
    static std::map<std::string, std::string> by_key;  // the shortest table key for each spelling-free key
    if (by_key.empty())
        for (const auto& [k, v] : names)
            if (auto [it, fresh] = by_key.try_emplace(trick_key(k), k); !fresh && k.size() < it->second.size()) it->second = k;
    if (auto it = by_key.find(trick_key(id)); it != by_key.end()) return "#" + it->second;
    return "#ID_TRICK_" + id;
}

ComboState::Seg name_kind(const std::string& n) {
    using Seg = ComboState::Seg;
    if (n.find("GRIND") != std::string::npos || n.find("SLIDE") != std::string::npos) return Seg::Grind;
    if (n.find("MANUAL") != std::string::npos) return Seg::Manual;
    if (n.find("PLANT") != std::string::npos || n.find("BONELESS") != std::string::npos) return Seg::Plant;
    return Seg::Air;
}

void score_events(Bridge& b, uint8_t* rdram, const recomp_context* ctx, uint32_t sk,
                  const s3::game::driver::TickRecord& r, ComboState& cs) {
    static const bool log = std::getenv("THPS_SKATE_LOG") != nullptr;
    using Seg = ComboState::Seg;
    auto combo_active = [&] { return s32(rdram, sk + 0x858) != 0; };
    const uint32_t list = sk + kComboList;
    bool bailed = false;
    std::string performed;  // a trick performed this tick (TRICK fires for every trick, a repeat of the last too)
    for (const std::string& e : r.events) {
        if (log && e.rfind("CONTACT", 0) != 0 && e.rfind("CLIP:", 0) != 0) std::fprintf(stderr, "[skate] event %s\n", e.c_str());
        if (e == "BAIL") bailed = true;
        if (e.rfind("TRICK:", 0) == 0) {
            performed = trick_score_name(b, e.substr(6));
            if (!log) std::fprintf(stderr, "[skate] trick %s -> %s\n", e.c_str() + 6, performed.c_str());
        }
    }
    const std::string& st = r.physical_state;
    Seg now = Seg::None;
    if (r.wiping_out || st == "WipeoutGround") now = Seg::None;
    else if (st == "PhysicsAir" || st == "KnownAir" || st == "PhysicsAirSecondary" || st == "Boneless") now = Seg::Air;
    else if (st.rfind("Grind", 0) == 0 || st == "SlideGround") now = Seg::Grind;
    else if (st == "HandPlant" || st == "FootPlant") now = Seg::Plant;
    else if (r.motion_state.find("Manual") != std::string::npos) now = Seg::Manual;
    // A segment survives a few ticks without its state (grind and manual states flicker for a tick or two).
    if (now == Seg::None && cs.seg != Seg::None && !r.wiping_out && cs.gap < 6) {
        cs.gap++;
        now = cs.seg;
    } else if (now == cs.seg) {
        cs.gap = 0;
    }

    if (bailed) {
        if (combo_active()) call_game(func_8004CED0, rdram, ctx, sk);
        if (cs.in_combo) std::fprintf(stderr, "[skate] combo lost\n");
        const std::string shown = r.score_name;
        cs = ComboState{};
        cs.announced = shown;
        return;
    }
    // What the skater is doing gets named by two signals: a TRICK event (every trick, repeats included; its id
    // gives the base name) and Skate 3's displayed score name, which it only re-announces when it changes and
    // which adds the spin and body flips ("... 360 BACKFLIP"). Either can arrive a tick before the air or grind
    // state starts, so it waits as pending for the segment it belongs to.
    std::string announced;
    if (!performed.empty()) announced = performed;
    if (r.score_name != cs.announced && !r.score_name.empty() && r.score_name != "#ID_TRICK_AIR") announced = r.score_name;
    cs.announced = r.score_name;
    cs.pending_age++;
    if (!announced.empty()) {
        if (name_kind(announced) == now && now != Seg::None) cs.name = announced;
        else {
            cs.pending = announced;
            cs.pending_age = 0;
        }
    }
    if (now != cs.seg) {
        cs.seg = now;
        cs.name.clear();
        cs.ticks = 0;
        cs.rolling = 0;
        cs.entry = -1;
        cs.shown.clear();
        cs.shown_points = -1;
        cs.gap = 0;
        if (now != Seg::None && cs.pending_age < 20 && name_kind(cs.pending) == now)
            cs.name = cs.pending;
    }
    if (now == Seg::None) {
        // Rolling: bank once nothing follows within the grace (Skate 3's landing frames, a quick re-pop).
        if (cs.in_combo && ++cs.rolling > 20) {
            if (combo_active()) {
                w32(rdram, sk + 0x424, 0);  // spins are in the trick names and points; no THPS spin bonus on top
                w32(rdram, sk + 0x420, 0);
                call_game(func_8004CC10, rdram, ctx, sk);
            }
            std::fprintf(stderr, "[skate] combo banked\n");
            cs.in_combo = false;
        }
        return;
    }
    cs.ticks++;
    // An air only counts once it is a trick (rolling off a ledge is not); grinds, manuals and plants at once.
    std::string sn = cs.name;
    if (sn.empty() || sn == "#ID_TRICK_AIR") {
        if (now == Seg::Air) return;
        sn = now == Seg::Manual ? "#ID_TRICK_GROUND_TRICK_MANUAL" : now == Seg::Grind ? "#ID_TRICK_CATEGORY_GRIND"
                                                                                       : "#ID_TRICK_BS_HANDPLANT";
    }
    const std::string name = skate3_name(b, sn);
    const int pts = segment_points(now, sn, cs.ticks);
    if (cs.entry < 0) {
        if (!combo_active()) call_game(func_8004C9E8, rdram, ctx, sk);
        if (!combo_active()) return;
        const std::string key = sn.substr(0, sn.find(' '));
        call_game(func_8004CACC, rdram, ctx, sk, trick_slot(key), game_string(rdram, name), pts);
        const int count = s32(rdram, list + kComboCount);
        if (count <= 0) return;
        cs.entry = count - 1;
        const int stored = s32(rdram, list + kComboEntries + uint32_t(cs.entry) * 20 + 4);
        cs.ratio = pts > 0 ? float(stored) / float(pts) : 1.0f;
        cs.shown = name;
        cs.shown_points = pts;
        cs.in_combo = true;
        std::fprintf(stderr, "[skate] + %s (%d)\n", name.c_str(), stored);
        return;
    }
    if (name == cs.shown && pts == cs.shown_points) return;
    if (s32(rdram, list + kComboCount) != cs.entry + 1) return;  // the list moved on (banked elsewhere)
    const uint32_t e = list + kComboEntries + uint32_t(cs.entry) * 20;
    w32(rdram, e, game_string(rdram, name));
    w32(rdram, e + 4, uint32_t(int32_t(std::lround(float(pts) * cs.ratio))));
    call_game(func_80045A70, rdram, ctx, list);
    if (log && name != cs.shown) std::fprintf(stderr, "[skate] = %s\n", name.c_str());
    cs.shown = name;
    cs.shown_points = pts;
}

// ----------------------------------------------------------------- sounds ----
// THPS plays the skater's sounds from inside the physics routine the bridge replaces (THPS64 decomp):
//   func_80058E3C(skater)          the loops, each frame: rolling (state 0, chosen by the terrain +0x824), air
//                                  wind (1/2), grind (4), wallride (5); volume and pitch from the speed +0x84
//   func_80039E60(id, 10000, 0)    one-shots: 6 ollie, 0x22 push, landing by terrain (2 -> 0x26, 10/12 -> 0x23,
//                                  6 none, otherwise 5), 9 / 10 bail
// The terrain is the face word's bits 25..28 (THPS: (halfword +0xC & 0x1E00) >> 9 of the face it stands on).
extern "C" {
void func_80058E3C(uint8_t* rdram, recomp_context* ctx);
void func_80039E60(uint8_t* rdram, recomp_context* ctx);
}

struct SoundCues { bool pop = false, land = false, bail = false, push = false; };

// Terrain of the solid face under (or at) a point, or -1.
int ground_terrain(const Bridge& b, const Vec3& at) {
    const Vec3 p{at[0], at[1] - 40.0f, at[2]}, q{at[0], at[1] + 120.0f, at[2]};  // PSX y points down
    float best = 2.0f;
    int terrain = -1;
    for (const auto& t : b.geo.tris) {
        if (!t.solid()) continue;
        const float lo = std::min({t.v[0][1], t.v[1][1], t.v[2][1]}), hi = std::max({t.v[0][1], t.v[1][1], t.v[2][1]});
        if (hi < p[1] || lo > q[1]) continue;
        if (std::min({t.v[0][0], t.v[1][0], t.v[2][0]}) > at[0] || std::max({t.v[0][0], t.v[1][0], t.v[2][0]}) < at[0]) continue;
        if (std::min({t.v[0][2], t.v[1][2], t.v[2][2]}) > at[2] || std::max({t.v[0][2], t.v[1][2], t.v[2][2]}) < at[2]) continue;
        // Vertical segment against the triangle in the xz plane (barycentric), then the hit height.
        const float x0 = t.v[0][0], z0 = t.v[0][2];
        const float ax = t.v[1][0] - x0, az = t.v[1][2] - z0, bx = t.v[2][0] - x0, bz = t.v[2][2] - z0;
        const float det = ax * bz - az * bx;
        if (std::fabs(det) < 1e-4f) continue;
        const float u = ((at[0] - x0) * bz - (at[2] - z0) * bx) / det;
        const float v = (ax * (at[2] - z0) - az * (at[0] - x0)) / det;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
        const float y = t.v[0][1] + u * (t.v[1][1] - t.v[0][1]) + v * (t.v[2][1] - t.v[0][1]);
        const float f = (y - p[1]) / (q[1] - p[1]);
        if (f < 0.0f || f > 1.0f || f >= best) continue;
        best = f;
        terrain = int((t.face >> 25) & 0xFu);
    }
    return terrain;
}

void skater_sounds(Bridge& b, uint8_t* rdram, const recomp_context* ctx, uint32_t sk, const SoundCues& cues) {
    static const bool off = std::getenv("THPS_SKATE_SOUND") && *std::getenv("THPS_SKATE_SOUND") == '0';
    if (off) return;
    const int state = s32(rdram, sk + 0x82C);
    if (state == 0 || cues.land) {
        const int t = ground_terrain(b, skater_pos_units(rdram, sk));
        static const bool log = std::getenv("THPS_SKATE_LOG") != nullptr;
        if (log && t != s32(rdram, sk + 0x824)) std::fprintf(stderr, "[skate] terrain %d\n", t);
        if (t >= 0) w32(rdram, sk + 0x824, uint32_t(t));
    }
    // THPS's bail flag (+0x450) mutes the rolling loop; the Skate 3 wipeout is state 7, which it plays nothing for.
    w32(rdram, sk + 0x450, 0);
    auto play = [&](uint32_t id) { call_game(func_80039E60, rdram, ctx, id, 10000, 0); };
    if (cues.pop) play(6);
    if (cues.push && state == 0) play(0x22);
    if (cues.land) {
        switch (s32(rdram, sk + 0x824)) {
            case 2: play(0x26); break;
            case 10: case 12: play(0x23); break;
            case 6: break;
            default: play(5); break;
        }
    }
    if (cues.bail) play(10);
    call_game(func_80058E3C, rdram, ctx, sk);
}

}  // namespace

// Catch recorder: every bail, and every sudden loss of over half the board's speed (from 4 m/s or more, in 3 ticks)
// that is not a landing, is appended to ~/.config/thpskate3/catches.log with the level, the THPS position, speeds
// and states, and the next frame is saved next to it as catches/catch_<n>.ppm. THPS_SKATE_CATCHES=0 turns it off.
namespace RT64 { void RequestScreenshot(const char* path); }
struct CatchRecorder {
    std::array<float, 4> speed{};
    std::array<std::string, 4> state;
    int frames_since = 1000;
    int count = 0;
    bool enabled = [] { const char* e = std::getenv("THPS_SKATE_CATCHES"); return !e || std::atoi(e) != 0; }();
};
void record_catch(CatchRecorder& c, const std::string& level, float units, const s3::game::driver::TickRecord& r, bool bail) {
    const auto& v = r.board.back().linear_velocity;
    const float sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (size_t k = 3; k > 0; k--) {
        c.speed[k] = c.speed[k - 1];
        c.state[k] = c.state[k - 1];
    }
    c.speed[0] = sp;
    c.state[0] = r.physical_state;
    c.frames_since++;
    if (!c.enabled || c.frames_since < 60) return;
    bool landed = false;
    for (const auto& e : r.events) landed = landed || e.rfind("LANDED", 0) == 0;
    const bool stop = c.speed[3] > 4.0f && sp < 0.5f * c.speed[3] && !landed && c.state[3] != "PhysicsAir" && c.state[3] != "KnownAir";
    if (!bail && !stop) return;
    c.frames_since = 0;
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "thpskate3" : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(dir / "catches", ec);
    const auto& p = r.board.back().position;
    const auto t = thpskate::from_s3({p[0], p[1], p[2]}, units);
    const auto& q = r.board.back().orientation;
    const Vec3 f = quat_rotate(q, {0, 0, 1});
    const int n = c.count++;
    const std::string shot = (dir / "catches" / ("catch_" + std::to_string(n) + ".ppm")).string();
    if (FILE* fp = std::fopen((dir / "catches.log").string().c_str(), "a")) {
        std::fprintf(fp, "catch %d %s level=%s pos=%.0f,%.0f,%.0f heading=%.3f speed=%.2f->%.2f state=%s->%s units=%.0f shot=%s\n", n,
                     bail ? "BAIL" : "STOP", level.c_str(), t[0], t[1], t[2], std::atan2(f[0], f[2]), c.speed[3], sp,
                     c.state[3].c_str(), r.physical_state.c_str(), units, shot.c_str());
        std::fclose(fp);
    }
    RT64::RequestScreenshot(shot.c_str());
}

// The run clock lives in the physics routine the bridge replaces (func_80059BEC): once the skater's end time (+0x428)
// is behind the frame counter (D_800DE994) it plays the time-up sound and calls func_80082660, which raises
// D_800DE9AC; func_80036664 then ends the run when the skater is on the ground (+0x82C == 0).
extern "C" void func_80082660(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80039E60(uint8_t* rdram, recomp_context* ctx);
void run_clock(uint8_t* rdram, recomp_context* ctx, uint32_t sk) {
    const int32_t mode = s32(rdram, 0x800DE7A0);
    if (mode == 2) return;
    const int32_t now = s32(rdram, 0x800DE994);
    const bool countdown = s32(rdram, 0x800DE7B0) != 0;
    const int32_t left = countdown ? 100 - now : s32(rdram, sk + 0x428) - now;
    if (left >= 0 || s32(rdram, 0x800DE9AC) != 0) return;
    if (countdown) {
        w32(rdram, 0x800DE804, 8);
        return;
    }
    if (mode == 1 && s32(rdram, kLevelTable + uint32_t(s32(rdram, 0x800C0E20)) * 0x78 + 0xC) != 0)
        call_game(func_80039E60, rdram, ctx, 0xA0, 10000, 0);
    call_game(func_80082660, rdram, ctx, 0);
}

extern "C" int thps_skate_physics(uint8_t* rdram, recomp_context* ctx) {
    Bridge& b = bridge();
    if (!b.enabled || b.failed) return 0;
    const uint32_t sk = uint32_t(ctx->r4);
    if (!ram_ptr(sk)) return 0;
    if (!load_pack(b)) {
        b.failed = true;
        return 0;
    }
    const std::string level = current_level(rdram);
    if (level.empty()) return 0;
    run_clock(rdram, ctx, sk);
    // A background rebuild: hold the skater still (THPS keeps drawing the last pose) until it is ready.
    if (b.build.valid()) {
        if (b.build.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 1;
        auto loaded = b.build.get();
        if (level == b.build_level && sk == b.build_skater) {
            if (!finish_spawn(b, std::move(loaded), b.build_level, b.build_skater, b.build_units)) {
                b.failed = true;
                return 0;
            }
            return 1;
        }
    }
    // (Re)spawn on a new level or skater, or when THPS itself moved the skater (a restart, a new run).
    bool teleported = false;
    if (b.sim && b.have_written && sk == b.skater) {
        for (int i = 0; i < 3; i++)
            teleported = teleported ||
                         std::abs(s32(rdram, sk + 4 + 4 * uint32_t(i)) - b.written[size_t(i)]) > (64 << 12);
    }
    if (!b.sim || level != b.level || sk != b.skater || teleported) {
        // On a restart point (a new run, a restart), spawn at once facing the restart's way. Anywhere else let
        // THPS place and orient the skater for a second first (its physics moves it to the restart point).
        float heading = 0.0f;
        const bool at_restart = restart_heading(b, level, skater_pos_units(rdram, sk), heading);
        static uint32_t settle_skater = 0;
        static int settle = 0;
        if (!at_restart) {
            if (settle_skater != sk || b.sim) {
                settle_skater = sk;
                settle = 30;
                b.sim.reset();
            }
            if (settle > 0) {
                settle--;
                return 0;
            }
        }
        settle_skater = 0;
        if (!spawn(b, rdram, sk, level, at_restart ? &heading : nullptr)) {
            b.failed = true;
            return 0;
        }
    }
    // Trainer settings: tuning and gravity (re-applied after every spawn), camera, game speed, markers.
    const thps::trainer::Settings ts = thps::trainer::get();
    // Skater size: the Skate 3 world is rebuilt at the new scale (units per metre), the skater respawned where it
    // is, facing the same way, and the character recalibrated.
    if (!b.units_from_env) {
        const float units = thpskate::kUnitsPerMetre * std::clamp(ts.skater_size, 0.5f, 3.0f);
        // Wait for the slider to rest (half a second) so dragging it respawns once, not every step.
        if (std::fabs(units - b.pending_units) > 0.01f) {
            b.pending_units = units;
            b.pending_frames = 15;
        } else if (b.pending_frames > 0) {
            b.pending_frames--;
        }
        if (std::fabs(units - b.units) > 0.01f && b.pending_frames == 0) {
            float heading = skater_heading(rdram, sk);
            if (b.have_presentation) {
                const Vec3 f = quat_rotate(b.last_deck, {0, 0, 1});
                heading = std::atan2(f[0], f[2]);
            }
            if (!spawn(b, rdram, sk, level, &heading, true, units)) {
                b.failed = true;
                return 0;
            }
            std::fprintf(stderr, "[skate] skater size x%.2f\n", ts.skater_size);
            return 1;
        }
    }
    b.camera = ts.skate3_camera && !(std::getenv("THPS_SKATE_CAM") && *std::getenv("THPS_SKATE_CAM") == '0');
    if (!b.tuning_applied || !(ts == b.applied)) {
        s3::game::trainer::TrainerTuning t;
        t.pop = ts.pop;
        t.grind_pop = ts.grind_pop;
        t.push_speed = ts.push_speed;
        t.push_power = ts.push_power;
        t.braking = ts.braking;
        t.steering = ts.steering;
        t.wobble = ts.wobble;
        t.offboard_jump = ts.offboard_jump;
        t.grip = ts.grip;
        t.turn_power = ts.turn_power;
        t.manual_drag = ts.manual_drag;
        t.hold_fakie = ts.hold_fakie;
        b.sim->set_tuning(t, ts.gravity);
        b.applied = ts;
        b.tuning_applied = true;
    }
    // Session marker on the pad (Skate 3's buttons): LB + d-pad down sets it, LB + d-pad up goes back to it,
    // LB + d-pad left / right picks the trainer slot it lives in. Auto-return goes back ~2.5 s after a bail.
    if (!thps::trainer::is_open()) {
        const uint16_t pb = thps::xpad::get(0).buttons;
        const uint16_t pressed = uint16_t(pb & ~b.session_buttons);
        b.session_buttons = pb;
        if (pb & thps::xpad::LEFT_SHOULDER) {
            const int sel = thps::trainer::selected_slot();
            if (pressed & thps::xpad::DPAD_DOWN) thps::trainer::request_marker(sel, true);
            if ((pressed & thps::xpad::DPAD_UP) && b.markers[size_t(sel)].set) thps::trainer::request_marker(sel, false);
            if (pressed & thps::xpad::DPAD_LEFT) thps::trainer::select_slot(sel - 1);
            if (pressed & thps::xpad::DPAD_RIGHT) thps::trainer::select_slot(sel + 1);
            if (pressed & (thps::xpad::DPAD_LEFT | thps::xpad::DPAD_RIGHT))
                std::fprintf(stderr, "[skate] session slot %d\n", thps::trainer::selected_slot() + 1);
        }
        if (b.bail_timer > 0 && --b.bail_timer == 0 && ts.auto_return) {
            const int sel = thps::trainer::selected_slot();
            if (b.markers[size_t(sel)].set && b.markers[size_t(sel)].level == level) thps::trainer::request_marker(sel, false);
        }
    }
    {
        int slot = 0;
        bool save = false;
        if (thps::trainer::take_marker_request(slot, save) && slot >= 0 && slot < thps::trainer::kMarkers) {
            auto& m = b.markers[size_t(slot)];
            if (save) {
                m.set = true;
                m.pos = skater_pos_units(rdram, sk);
                const auto& deck = b.have_presentation ? b.last_deck : std::array<float, 4>{0, 0, 0, 1};
                const Vec3 f = quat_rotate(deck, {0, 0, 1});
                m.heading = std::atan2(f[0], f[2]);
                m.level = level;
                thps::trainer::set_marker_saved(slot, true);
                std::fprintf(stderr, "[skate] session marker %d set at (%.0f %.0f %.0f)\n", slot + 1, m.pos[0], m.pos[1], m.pos[2]);
            } else if (m.set && m.level == level) {
                std::fprintf(stderr, "[skate] back to session marker %d\n", slot + 1);
                for (int i = 0; i < 3; i++)
                    w32(rdram, sk + 4 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(double(m.pos[size_t(i)]) * 4096.0))));
                if (!spawn(b, rdram, sk, level, &m.heading, true)) {
                    b.failed = true;
                    return 0;
                }
                return 1;
            }
        }
    }
    // THPS_SKATE_SPAWN="x,y,z,heading" (testing): once, move the skater there (THPS units; Skate 3 heading, radians).
    if (const char* sp = std::getenv("THPS_SKATE_SPAWN")) {
        static bool done = false;
        float x = 0, y = 0, z = 0, h = 0;
        if (!done && std::sscanf(sp, "%f,%f,%f,%f", &x, &y, &z, &h) == 4) {
            done = true;
            const float p[3] = {x, y, z};
            for (int i = 0; i < 3; i++) w32(rdram, sk + 4 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(double(p[i]) * 4096.0))));
            if (!spawn(b, rdram, sk, level, &h)) {
                b.failed = true;
                return 0;
            }
            return 1;
        }
    }
    // THPS updates the skater at 30 Hz; skatecore runs its fixed 60 Hz tick twice (game speed scales that).
    const Pad pad = thps::trainer::is_open() ? Pad{} : pad_from_xpad(0);
    b.tick_accumulator += 2.0f * std::clamp(ts.game_speed, 0.05f, 2.0f);
    const int ticks = int(b.tick_accumulator);
    b.tick_accumulator -= float(ticks);
    s3::game::driver::TickRecord last{};
    SoundCues cues;
    if (ticks == 0) return 1;  // slow motion: nothing to step this frame (THPS keeps the last pose)
    for (int i = 0; i < ticks; i++) {
        auto step = b.sim->step(pad);
        if (step.is_err()) {
            std::fprintf(stderr, "[skate] step failed: %s; respawning\n", step.unwrap_err().c_str());
            b.sim.reset();
            return 1;
        }
        last = std::move(step.unwrap());
        static ComboState combo;
        score_events(b, rdram, ctx, sk, last, combo);
        bool bailed = false;
        for (const std::string& e : last.events) {
            if (e == "AIRBORNE") cues.pop = true;
            else if (e.rfind("LANDED", 0) == 0) cues.land = true;
            else if (e == "BAIL") cues.bail = bailed = true;
        }
        static CatchRecorder catches;
        record_catch(catches, level, b.units, last, bailed);
    }
    b.last = b.sim->presentation();
    b.have_presentation = true;
    b.last_deck = last.board.back().orientation;
    write_skater(b, rdram, sk, last);
    {
        // A push: the foot's push speed rising from rest (presentation, audio observation).
        const bool pushing = b.last.foot_push_speed > 0.05f;
        cues.push = pushing && !b.was_pushing;
        b.was_pushing = pushing;
        skater_sounds(b, rdram, ctx, sk, cues);
        if (cues.bail) b.bail_timer = 75;
    }
    {
        if (!last.score_name.empty()) b.last_trick = skate3_name(b, last.score_name);
        const auto& v = last.board.back().linear_velocity;
        thps::trainer::Status st;
        st.speed_kmh = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 3.6f;
        st.state = last.physical_state;
        st.trick = b.last_trick;
        st.level = level;
        thps::trainer::set_status(st);
    }
    run_triggers(b, rdram, ctx, skater_pos_units(rdram, sk));
    // THPS_SKATE_GAPTEST="start,end" (hex keys, testing): ~3 s after a spawn, run a combo through those two
    // trigger keys as if the skater had crossed both planes in the air, then land it.
    if (const char* gt = std::getenv("THPS_SKATE_GAPTEST")) {
        static int frames = 0;
        static int stage = 0;
        const uint32_t k1 = uint32_t(std::strtoul(gt, nullptr, 16));
        const char* comma = std::strchr(gt, ',');
        const uint32_t k2 = comma ? uint32_t(std::strtoul(comma + 1, nullptr, 16)) : 0;
        frames++;
        const uint32_t who = sk == r32(rdram, kCurrentSkater) ? 1u : 2u;
        auto fire = [&](uint32_t key) {
            call_game(func_80061E7C, rdram, ctx, who);
            w32(rdram, kTriggerSkater, sk);
            call_game(func_80061D7C, rdram, ctx, key, 1);
            call_game(func_80061E34, rdram, ctx, who);
            w32(rdram, kTriggerSkater, 0);
        };
        if (stage == 0 && frames > 90) {
            call_game(func_8004C9E8, rdram, ctx, sk);
            std::fprintf(stderr, "[gaptest] combo %d score %d\n", s32(rdram, sk + 0x858), s32(rdram, sk + 0x2E8));
            fire(k1);
            stage = 1;
        } else if (stage == 1 && frames > 95) {
            fire(k2);
            std::fprintf(stderr, "[gaptest] after end trigger: combo %d\n", s32(rdram, sk + 0x858));
            stage = 2;
        } else if (stage == 2 && frames > 100) {
            call_game(func_8004CC10, rdram, ctx, sk);
            std::fprintf(stderr, "[gaptest] landed: score %d\n", s32(rdram, sk + 0x2E8));
            stage = 3;
        }
    }
    return 1;
}

extern "C" int thps_skate_camera(uint8_t* rdram, recomp_context* ctx) {
    Bridge& b = bridge();
    if (!b.camera || !b.enabled || b.failed || !b.sim || !b.have_presentation || !b.last.has_camera) return 0;
    const uint32_t o = uint32_t(ctx->r4);
    if (!ram_ptr(o)) return 0;
    const auto& pres = b.last;
    const Vec3 eye = thpskate::from_s3({pres.camera_position[0], pres.camera_position[1], pres.camera_position[2]},
                                       b.units);
    // Forward and up from skatecore's camera frame; right is rebuilt so that right x down = forward, the
    // proper (det +1) PSX camera frame a quaternion can represent.
    const Vec3 fwd = norm(flip_yz({pres.camera_basis[2][0], pres.camera_basis[2][1], pres.camera_basis[2][2]}));
    const Vec3 up0 = norm(flip_yz({pres.camera_basis[1][0], pres.camera_basis[1][1], pres.camera_basis[1][2]}));
    const Vec3 right = norm(cross({-up0[0], -up0[1], -up0[2]}, fwd));
    const Vec3 down = cross(fwd, right);
    const Vec3 at{eye[0] + fwd[0] * 400.0f, eye[1] + fwd[1] * 400.0f, eye[2] + fwd[2] * 400.0f};
    for (int i = 0; i < 3; i++) {
        w32(rdram, o + 4 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(double(eye[size_t(i)]) * 4096.0))));
        w32(rdram, o + 0x6F0 + 4 * uint32_t(i), 0);
        w32(rdram, o + 0x500 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(double(at[size_t(i)]) * 4096.0))));
    }
    const auto q = quat_from_rows(right, down, fwd);
    for (int i = 0; i < 4; i++)
        w32(rdram, o + 0x554 + 4 * uint32_t(i), uint32_t(int32_t(std::lround(q[size_t(i)] * 4096.0f))));
    static int n = 0;
    if (n++ % 90 == 0) {
        const Vec3 sk = skater_pos_units(rdram, b.skater);
        std::fprintf(stderr, "[skate] cam eye (%.0f %.0f %.0f) fwd (%.2f %.2f %.2f) skater (%.0f %.0f %.0f) fov %.1f\n",
                     eye[0], eye[1], eye[2], fwd[0], fwd[1], fwd[2], sk[0], sk[1], sk[2], pres.field_of_view_degrees);
    }
    return 0;  // THPS's func_80073EA0 now builds the view from the camera object
}

// ------------------------------------------------------------------- pose ----
// THPS characters, animated by Skate 3. Every THPS skater mesh (hawk.psx, Lasek.PSX, ...) is skinned to the same
// 19-bone rig; func_80084CB8(model, world, hidden) plays THPS's animation and writes one N64 Mtx per bone
// (model-space bone x world, scaled 1/16) to model + parity * 0x600, the buffer the mesh draws with (segment 1).
// Right after it returns, every bone is rewritten from skatecore's animated skeleton:
//     thps_world(bone) = s3_world(source bone) * offset(bone)
// where offset() is measured on the first frame after a spawn, when THPS has just posed its own skater (standing
// on the board) exactly where the Skate 3 skater stands. That absorbs both rigs' axis conventions, proportions,
// the PSX mirror and the unit scale. Left/right chains are matched to whichever Skate 3 side is nearer then.
//
// Roles of the THPS bones (measured on Tony Hawk; tools/thps_skel.py): 0 deck, 1/2 trucks, 3 pelvis, 4/6/5 and
// 7/9/8 the legs (thigh, shin, foot), 10 lower torso, 11 chest, 14/13/12 and 17/16/15 the arms (upper arm,
// forearm, hand), 18 head.

namespace {

using M4 = std::array<double, 16>;  // column-major, column vectors (p' = M p)

M4 m4_identity() { M4 m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
M4 m4_mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += a[size_t(k * 4 + rr)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + rr)] = v;
        }
    return r;
}
// Affine inverse (the 3x3 part may be scaled or mirrored).
M4 m4_inverse_affine(const M4& m) {
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    M4 r = m4_identity();
    if (std::fabs(det) < 1e-12) return r;
    const double inv[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
                           B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
                           C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
    // inv is row-major; store column-major
    for (int rr = 0; rr < 3; rr++)
        for (int cc = 0; cc < 3; cc++) r[size_t(cc * 4 + rr)] = inv[rr * 3 + cc];
    for (int rr = 0; rr < 3; rr++)
        r[size_t(12 + rr)] = -(r[size_t(rr)] * m[12] + r[size_t(4 + rr)] * m[13] + r[size_t(8 + rr)] * m[14]);
    return r;
}
M4 m4_from_native(const std::array<std::array<float, 4>, 4>& m) {  // skatecore native: [column][row]
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) r[size_t(c * 4 + rr)] = m[size_t(c)][size_t(rr)];
    r[3] = r[7] = r[11] = 0;  // the native 4th lanes are not part of the transform
    r[15] = 1;
    return r;
}

// N64 Mtx (s15.16, row-vector: p' = p M, translation in row 3) <-> column-major.
M4 n64_read(const uint8_t* rdram, uint32_t a) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            const uint32_t k = uint32_t(i * 4 + j);
            int16_t hi;
            uint16_t lo;
            std::memcpy(&hi, rdram + (((a + 2 * k) ^ 2) - 0x80000000u), 2);
            std::memcpy(&lo, rdram + (((a + 32 + 2 * k) ^ 2) - 0x80000000u), 2);
            r[size_t(i * 4 + j)] = hi + lo / 65536.0;  // row-vector [i][j] == column-major element (row j, col i)
        }
    return r;
}
void n64_write(uint8_t* rdram, uint32_t a, const M4& m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            const uint32_t k = uint32_t(i * 4 + j);
            double v = m[size_t(i * 4 + j)];
            v = v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v);
            const int32_t fx = int32_t(std::floor(v * 65536.0));
            const int16_t hi = int16_t(fx >> 16);
            const uint16_t lo = uint16_t(fx & 0xFFFF);
            std::memcpy(rdram + (((a + 2 * k) ^ 2) - 0x80000000u), &hi, 2);
            std::memcpy(rdram + (((a + 32 + 2 * k) ^ 2) - 0x80000000u), &lo, 2);
        }
}

// THPS character rigs (tools/thps_skel.py export_rigs -> rigs.thr), keyed by the mesh pack index the model keeps
// at +0xC08: per bone the bone-local centroid and vertices, and the skin seams between parent and child bones.
struct Rig {
    uint32_t mesh_pack = 0;
    std::vector<Vec3> centroids;
    struct Seam { int parent, child; std::vector<Vec3> parent_side, child_side; };
    std::vector<Seam> seams;
    std::vector<std::vector<Vec3>> vertices;
};

const Rig* character_rig(Bridge& b, uint32_t mesh_pack) {
    static std::vector<Rig> rigs;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(b.levels_dir / "rigs.thr", std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t o = 0;
        auto u32 = [&]() { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); o += 4; return v; };
        auto vec = [&]() { Vec3 v{}; if (o + 12 <= d.size()) std::memcpy(v.data(), d.data() + o, 12); o += 12; return v; };
        if (d.size() >= 8 && std::memcmp(d.data(), "THR3", 4) == 0) {
            o = 4;
            const uint32_t n = u32();
            for (uint32_t i = 0; i < n && o < d.size(); i++) {
                Rig r;
                r.mesh_pack = u32();
                const uint32_t nb = u32();
                for (uint32_t k = 0; k < nb; k++) r.centroids.push_back(vec());
                const uint32_t ns = u32();
                for (uint32_t k = 0; k < ns; k++) {
                    Rig::Seam sm;
                    sm.parent = int(u32());
                    sm.child = int(u32());
                    const uint32_t na = u32(), nc = u32();
                    for (uint32_t j = 0; j < na; j++) sm.parent_side.push_back(vec());
                    for (uint32_t j = 0; j < nc; j++) sm.child_side.push_back(vec());
                    r.seams.push_back(std::move(sm));
                }
                r.vertices.resize(nb);
                for (uint32_t k = 0; k < nb; k++) {
                    const uint32_t nv = u32();
                    for (uint32_t j = 0; j < nv; j++) r.vertices[k].push_back(vec());
                }
                rigs.push_back(std::move(r));
            }
        } else {
            std::fprintf(stderr, "[skate] no THR3 rigs.thr in %s (run tools/thps_skel.py export_rigs)\n",
                         b.levels_dir.c_str());
        }
    }
    for (const auto& r : rigs) if (r.mesh_pack == mesh_pack) return &r;
    return nullptr;
}

const std::vector<Vec3>* rig_centroids(Bridge& b, uint32_t mesh_pack) {
    const Rig* r = character_rig(b, mesh_pack);
    return r ? &r->centroids : nullptr;
}

const std::vector<std::string>& s3_bone_names(Bridge& b) {
    static std::vector<std::string> names;
    if (names.empty() && b.vfs) {
        auto banks = s3::data::AnimationBanks::load(*b.vfs, "assets");
        if (banks.ok && banks->banks.at(0)->hierarchy()) names = banks->banks.at(0)->hierarchy()->bone_names;
        for (auto& n : names) for (auto& ch : n) ch = char(std::toupper(uint8_t(ch)));
    }
    return names;
}

int s3_bone(Bridge& b, const char* name) {
    const auto& n = s3_bone_names(b);
    for (size_t i = 0; i < n.size(); i++) if (n[i] == name) return int(i);
    return -1;
}

uint32_t g_pose_model = 0;  // a0 of the func_80084CB8 call in progress

// skatecore world (metres, Y up) -> THPS world (units, PSX axes) as a matrix: diag(U, -U, -U).
M4 s3_to_thps(float units) {
    M4 f = m4_identity();
    f[0] = units;
    f[5] = -units;
    f[10] = -units;
    return f;
}

std::vector<M4> s3_world_bones(Bridge& b) {
    std::vector<M4> out;
    if (!b.have_presentation || b.last.bones == nullptr) return out;
    const M4 root = m4_mul(s3_to_thps(b.units), m4_from_native(b.last.root));
    for (const auto& m : *b.last.bones) out.push_back(m4_mul(root, m4_from_native(m)));
    return out;
}

Vec3 m4_point(const M4& m, const Vec3& p) {
    return {float(m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]),
            float(m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]),
            float(m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14])};
}

// How much bigger than THPS drew it the character is shown: the trainer's skater size (units per metre over 100).
double character_scale(const Bridge& b) { return double(b.units) / double(thpskate::kUnitsPerMetre); }

Vec3 v_sub(const Vec3& a, const Vec3& c) { return {a[0] - c[0], a[1] - c[1], a[2] - c[2]}; }
Vec3 v_add(const Vec3& a, const Vec3& c) { return {a[0] + c[0], a[1] + c[1], a[2] + c[2]}; }
Vec3 v_mul(const Vec3& a, float f) { return {a[0] * f, a[1] * f, a[2] * f}; }
float v_len(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Rotation taking direction a onto direction c, about the point `pivot`, as a matrix applied on the left.
M4 rotate_onto(const Vec3& a, const Vec3& c, const Vec3& pivot) {
    const float la = v_len(a), lc = v_len(c);
    M4 r = m4_identity();
    if (la < 1e-6f || lc < 1e-6f) return r;
    const Vec3 u = v_mul(a, 1.0f / la), w = v_mul(c, 1.0f / lc);
    Vec3 axis = cross(u, w);
    const float sn = v_len(axis), cs = std::clamp(dot(u, w), -1.0f, 1.0f);
    if (sn < 1e-6f) return r;  // parallel (an exact flip does not happen for a limb between frames)
    axis = v_mul(axis, 1.0f / sn);
    const double x = axis[0], y = axis[1], z = axis[2], C = cs, S = sn, t = 1 - C;
    const double m[3][3] = {{t * x * x + C, t * x * y - S * z, t * x * z + S * y},
                            {t * x * y + S * z, t * y * y + C, t * y * z - S * x},
                            {t * x * z - S * y, t * y * z + S * x, t * z * z + C}};
    for (int rr = 0; rr < 3; rr++)
        for (int cc = 0; cc < 3; cc++) r[size_t(cc * 4 + rr)] = m[rr][cc];
    for (int rr = 0; rr < 3; rr++)
        r[size_t(12 + rr)] = pivot[size_t(rr)] - (m[rr][0] * pivot[0] + m[rr][1] * pivot[1] + m[rr][2] * pivot[2]);
    return r;
}

bool calibrate_pose(Bridge& b, uint8_t* rdram, uint32_t model, uint32_t buffer, int nbones) {
    const auto s3w = s3_world_bones(b);
    if (s3w.empty()) return false;
    const auto* cents = rig_centroids(b, r32(rdram, model + 0xC08));
    // THPS bones in world units: the N64 matrices are world / 16.
    M4 s16 = m4_identity();
    s16[0] = s16[5] = s16[10] = 16;
    std::vector<M4> tw(static_cast<size_t>(nbones), m4_identity());
    for (int i = 0; i < nbones; i++) tw[size_t(i)] = m4_mul(s16, n64_read(rdram, buffer + 64 * uint32_t(i)));
    auto thps_centre = [&](int i) {
        const Vec3 c = cents && size_t(i) < cents->size() ? (*cents)[size_t(i)] : Vec3{0, 0, 0};
        return m4_point(tw[size_t(i)], c);
    };
    auto s3_pos = [&](int bi) { const M4& m = s3w[size_t(bi)]; return Vec3{float(m[12]), float(m[13]), float(m[14])}; };
    auto s3_mid = [&](int bi, int child) {
        const Vec3 a = s3_pos(bi);
        if (child < 0) return a;
        const Vec3 c = s3_pos(child);
        return Vec3{(a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2};
    };
    auto dist = [](const Vec3& a, const Vec3& c) {
        return std::sqrt((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2]));
    };
    auto B = [&](const char* n) { return s3_bone(b, n); };
    b.pose_source.assign(size_t(nbones), -1);
    auto set = [&](int thps, int s3) { if (thps < nbones && s3 >= 0) b.pose_source[size_t(thps)] = s3; };
    set(0, B("SKATEBOARD_ROOT"));
    set(3, B("HIPS"));
    set(10, B("SPINE1"));
    set(11, B("SPINE3"));
    set(18, B("HEAD"));
    // Chains matched as a whole to the nearer Skate 3 side: THPS bones, then the right and left Skate 3 bones
    // with the child each one's centre is measured to.
    struct Chain { std::array<int, 3> thps; std::array<const char*, 3> right, left; std::array<const char*, 3> rc, lc; };
    const Chain legs_a{{4, 6, 5}, {"RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT"}, {"LEFTUPLEG", "LEFTLEG", "LEFTFOOT"},
                       {"RIGHTLEG", "RIGHTFOOT", "RIGHTTOEBASE"}, {"LEFTLEG", "LEFTFOOT", "LEFTTOEBASE"}};
    const Chain arms_a{{14, 13, 12}, {"RIGHTARM", "RIGHTFOREARM", "RIGHTHAND"}, {"LEFTARM", "LEFTFOREARM", "LEFTHAND"},
                       {"RIGHTFOREARM", "RIGHTHAND", nullptr}, {"LEFTFOREARM", "LEFTHAND", nullptr}};
    const std::array<int, 3> legs_b{7, 9, 8}, arms_b{17, 16, 15};
    auto match = [&](const Chain& c, const std::array<int, 3>& other) {
        double straight = 0, crossed = 0;
        for (int k = 0; k < 3; k++) {
            const Vec3 r = s3_mid(B(c.right[size_t(k)]), c.rc[size_t(k)] ? B(c.rc[size_t(k)]) : -1);
            const Vec3 l = s3_mid(B(c.left[size_t(k)]), c.lc[size_t(k)] ? B(c.lc[size_t(k)]) : -1);
            straight += dist(thps_centre(c.thps[size_t(k)]), r) + dist(thps_centre(other[size_t(k)]), l);
            crossed += dist(thps_centre(c.thps[size_t(k)]), l) + dist(thps_centre(other[size_t(k)]), r);
        }
        for (int k = 0; k < 3; k++) {
            set(c.thps[size_t(k)], B(straight <= crossed ? c.right[size_t(k)] : c.left[size_t(k)]));
            set(other[size_t(k)], B(straight <= crossed ? c.left[size_t(k)] : c.right[size_t(k)]));
        }
    };
    match(legs_a, legs_b);
    match(arms_a, arms_b);
    {
        const int f = B("TRUCK_FRONT"), k = B("TRUCK_BACK");
        const bool straight = dist(thps_centre(1), s3_pos(f)) + dist(thps_centre(2), s3_pos(k)) <=
                              dist(thps_centre(1), s3_pos(k)) + dist(thps_centre(2), s3_pos(f));
        set(1, straight ? f : k);
        set(2, straight ? k : f);
    }
    b.pose_offset.assign(size_t(nbones), m4_identity());
    // The trainer's skater size draws the character that much bigger than THPS made it (about each bone's own
    // origin; the joints and anchors below are in bone space, so they scale with it).
    const double k = character_scale(b);
    M4 kscale = m4_identity();
    kscale[0] = kscale[5] = kscale[10] = k;
    for (int i = 0; i < nbones; i++) {
        const int src = b.pose_source[size_t(i)];
        if (src < 0 || size_t(src) >= s3w.size()) continue;
        b.pose_offset[size_t(i)] = m4_mul(m4_mul(m4_inverse_affine(s3w[size_t(src)]), tw[size_t(i)]), kscale);
    }
    // Every body piece keeps only the calibrated rotation; its vertex centroid is placed on a point along the
    // Skate 3 skeleton (A + t (B - A) between two joints), so the skin stays on the joints whatever the two rigs'
    // poses were at calibration. Those points are scaled about the skater's root to this character's height
    // (THPS skaters stand ~195 units, Skate 3's rig ~175 at our scale), which keeps joints and skin together.
    struct Anchor { int thps; const char* a; const char* b; float t; };
    const Anchor anchors[] = {
        {3, "HIPS", "HIPS", 0.0f}, {10, "SPINE1", "SPINE2", 0.5f}, {11, "SPINE3", "NECK", 0.5f},
        {18, "NECK", "HEAD", 2.0f},
    };
    auto limb_anchor = [&](int i) -> Anchor {
        const int src = b.pose_source[size_t(i)];
        const auto& names = s3_bone_names(b);
        const std::string n = src >= 0 && size_t(src) < names.size() ? names[size_t(src)] : "";
        static const char* const chain[][2] = {
            {"RIGHTUPLEG", "RIGHTLEG"}, {"RIGHTLEG", "RIGHTFOOT"}, {"RIGHTFOOT", "RIGHTTOEBASE"},
            {"LEFTUPLEG", "LEFTLEG"}, {"LEFTLEG", "LEFTFOOT"}, {"LEFTFOOT", "LEFTTOEBASE"},
            {"RIGHTARM", "RIGHTFOREARM"}, {"RIGHTFOREARM", "RIGHTHAND"}, {"LEFTARM", "LEFTFOREARM"},
            {"LEFTFOREARM", "LEFTHAND"}};
        for (const auto& c : chain) if (n == c[0]) return {i, c[0], c[1], 0.5f};
        if (n == "RIGHTHAND") return {i, "RIGHTFOREARM", "RIGHTHAND", 1.4f};
        if (n == "LEFTHAND") return {i, "LEFTFOREARM", "LEFTHAND", 1.4f};
        return {i, nullptr, nullptr, 0.0f};
    };
    std::vector<Anchor> all(std::begin(anchors), std::end(anchors));
    for (int i : {4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17}) all.push_back(limb_anchor(i));
    auto anchor_point = [&](const Anchor& an) {
        const Vec3 pa = s3_pos(B(an.a)), pb = s3_pos(B(an.b));
        return Vec3{pa[0] + an.t * (pb[0] - pa[0]), pa[1] + an.t * (pb[1] - pa[1]), pa[2] + an.t * (pb[2] - pa[2])};
    };
    b.pose_limb.assign(size_t(nbones), false);
    b.pose_centroid.assign(size_t(nbones), {0, 0, 0});
    b.pose_anchor.assign(size_t(nbones), {0, 0, 0});
    const Vec3 root = m4_point(m4_mul(s3_to_thps(b.units), m4_from_native(b.last.root)), {0, 0, 0});
    b.pose_scale = 1.0;
    for (const Anchor& an : all) {
        if (an.a == nullptr || an.thps >= nbones || B(an.a) < 0 || B(an.b) < 0) continue;
        const int src = b.pose_source[size_t(an.thps)];
        if (src < 0 || !cents || size_t(an.thps) >= cents->size()) continue;
        const Vec3 target = anchor_point(an);
        const Vec3 local = m4_point(m4_inverse_affine(s3w[size_t(src)]), target);
        b.pose_limb[size_t(an.thps)] = true;
        b.pose_centroid[size_t(an.thps)] = {(*cents)[size_t(an.thps)][0], (*cents)[size_t(an.thps)][1],
                                            (*cents)[size_t(an.thps)][2]};
        b.pose_anchor[size_t(an.thps)] = {local[0], local[1], local[2]};
        if (an.thps == 18) {
            // Height ratio from the head: THPS's head centre over the root vs the Skate 3 head point (y is down).
            const float thps_h = float(k) * (root[1] - thps_centre(18)[1]), s3_h = root[1] - target[1];
            if (s3_h > 1.0f && thps_h > 1.0f) b.pose_scale = std::clamp(double(thps_h / s3_h), 0.8, 1.4);
        }
    }
    // Joints of THPS's own skeleton: where each child piece meets its parent, measured now (both bones posed by
    // THPS) and kept in both bones' frames. Posing then chains them, so the skin cannot open at a joint and the
    // character keeps its own proportions. Skinned models have seam vertices (triangles spanning the two bones);
    // rigid-piece models get the midpoint of the closest vertices of the two pieces.
    b.pose_joints.clear();
    if (const Rig* rig = character_rig(b, r32(rdram, model + 0xC08))) {
        for (const auto& sm : rig->seams) {
            if (sm.parent >= nbones || sm.child >= nbones || sm.parent < 0 || sm.child < 0) continue;
            if (!b.pose_limb[size_t(sm.child)] && sm.child != 18) continue;  // body bones only (board stays rigid)
            Vec3 j{0, 0, 0};
            int n = 0;
            for (const auto& v : sm.parent_side) { const Vec3 w = m4_point(tw[size_t(sm.parent)], v); for (int k = 0; k < 3; k++) j[size_t(k)] += w[size_t(k)]; n++; }
            for (const auto& v : sm.child_side) { const Vec3 w = m4_point(tw[size_t(sm.child)], v); for (int k = 0; k < 3; k++) j[size_t(k)] += w[size_t(k)]; n++; }
            if (n == 0) {
                // closest vertex pairs of the two pieces
                std::vector<std::pair<float, Vec3>> pairs;
                for (const auto& va : rig->vertices[size_t(sm.parent)])
                    for (const auto& vb : rig->vertices[size_t(sm.child)]) {
                        const Vec3 a = m4_point(tw[size_t(sm.parent)], va), c = m4_point(tw[size_t(sm.child)], vb);
                        pairs.push_back({dist(a, c), Vec3{(a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2}});
                    }
                std::sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                for (size_t k = 0; k < pairs.size() && k < 4; k++, n++)
                    for (int q = 0; q < 3; q++) j[size_t(q)] += pairs[k].second[size_t(q)];
            }
            if (n == 0) continue;
            for (auto& x : j) x /= float(n);
            Bridge::Joint jt;
            jt.parent = sm.parent;
            jt.child = sm.child;
            const Vec3 pl = m4_point(m4_inverse_affine(tw[size_t(sm.parent)]), j);
            const Vec3 cl = m4_point(m4_inverse_affine(tw[size_t(sm.child)]), j);
            jt.parent_local = {pl[0], pl[1], pl[2]};
            jt.child_local = {cl[0], cl[1], cl[2]};
            b.pose_joints.push_back(jt);
        }
    }
    std::fprintf(stderr, "[skate] pose calibrated (%d bones, rig %s, height x%.2f, %zu joints):", nbones, cents ? "found" : "missing",
                 b.pose_scale, b.pose_joints.size());
    for (int i = 0; i < nbones; i++) std::fprintf(stderr, " %d<-%d", i, b.pose_source[size_t(i)]);
    std::fprintf(stderr, "\n");
    return true;
}

}  // namespace

extern "C" void thps_skate_pose_begin(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    g_pose_model = uint32_t(ctx->r4);
}

extern "C" void thps_skate_pose_end(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    static const bool env_on = [] { const char* e = std::getenv("THPS_SKATE_POSE"); return !(e && *e == '0'); }();
    const bool on = env_on && thps::trainer::get().skate3_pose;
    Bridge& b = bridge();
    const uint32_t model = g_pose_model;
    if (!on || !b.enabled || b.failed || !b.sim || !b.have_presentation || !ram_ptr(model)) return;
    if (model != b.skater + 0xF18) return;  // only the skater the sim drives
    const int nbones = int(r32(rdram, model + 0xC0C));
    if (nbones <= 0 || nbones > 24) return;
    const uint32_t buffer = model + r32(rdram, 0x800F32B8) * 0x600;
    if (!b.pose_calibrated) {
        b.pose_calibrated = calibrate_pose(b, rdram, model, buffer, nbones);
        return;  // this frame keeps THPS's own pose
    }
    const auto s3w = s3_world_bones(b);
    if (s3w.empty()) return;
    const Vec3 root = m4_point(m4_mul(s3_to_thps(b.units), m4_from_native(b.last.root)), {0, 0, 0});
    M4 inv16 = m4_identity();
    inv16[0] = inv16[5] = inv16[10] = 1.0 / 16.0;
    std::vector<M4> posed(static_cast<size_t>(nbones), m4_identity());
    std::vector<bool> have(static_cast<size_t>(nbones), false);
    for (int i = 0; i < nbones; i++) {
        const int src = b.pose_source[size_t(i)];
        if (src < 0 || size_t(src) >= s3w.size()) continue;
        M4 w = m4_mul(s3w[size_t(src)], b.pose_offset[size_t(i)]);
        if (size_t(i) < b.pose_limb.size() && b.pose_limb[size_t(i)]) {
            const auto& a = b.pose_anchor[size_t(i)];
            const auto& c = b.pose_centroid[size_t(i)];
            Vec3 target = m4_point(s3w[size_t(src)], {float(a[0]), float(a[1]), float(a[2])});
            for (int r = 0; r < 3; r++)
                target[size_t(r)] = float(root[size_t(r)] + b.pose_scale * (target[size_t(r)] - root[size_t(r)]));
            for (int r = 0; r < 3; r++)
                w[size_t(12 + r)] = target[size_t(r)] - (w[size_t(r)] * c[0] + w[size_t(4 + r)] * c[1] + w[size_t(8 + r)] * c[2]);
        }
        posed[size_t(i)] = w;
        have[size_t(i)] = true;
    }
    // Chain THPS's joints from the pelvis out: each child piece keeps its rotation and moves so that its side of
    // the joint lands on its parent's side.
    for (const auto& jt : b.pose_joints) {
        if (!have[size_t(jt.parent)] || !have[size_t(jt.child)]) continue;
        const Vec3 at = m4_point(posed[size_t(jt.parent)], {float(jt.parent_local[0]), float(jt.parent_local[1]), float(jt.parent_local[2])});
        M4& w = posed[size_t(jt.child)];
        for (int r = 0; r < 3; r++)
            w[size_t(12 + r)] = at[size_t(r)] - (w[size_t(r)] * jt.child_local[0] + w[size_t(4 + r)] * jt.child_local[1] +
                                                 w[size_t(8 + r)] * jt.child_local[2]);
    }
    // Rigid pieces (the board) are scaled about their own centre, not the model origin.
    if (const auto* cents = rig_centroids(b, r32(rdram, model + 0xC08))) {
        const double k = character_scale(b);
        for (int i = 0; i < nbones && size_t(i) < cents->size(); i++) {
            if (!have[size_t(i)] || (size_t(i) < b.pose_limb.size() && b.pose_limb[size_t(i)]) || k == 1.0) continue;
            M4& w = posed[size_t(i)];
            const Vec3& c = (*cents)[size_t(i)];
            const Vec3 now = m4_point(w, c), want = m4_point(w, v_mul(c, float(1.0 / k)));
            for (int r = 0; r < 3; r++) w[size_t(12 + r)] += want[size_t(r)] - now[size_t(r)];
        }
    }
    // Hands and feet: the chain above keeps THPS's limb lengths, so its wrists and ankles drift off where Skate 3
    // has them (on the deck, on the board in a grab, round the board when carrying it). Two-bone IK bends each
    // limb to reach Skate 3's wrist / ankle, in the plane of Skate 3's own elbow / knee; the hand or foot keeps
    // its Skate 3 rotation.
    static const bool ik = [] { const char* e = std::getenv("THPS_SKATE_IK"); return !(e && *e == '0'); }();
    if (ik) {
        const Rig* rig = character_rig(b, r32(rdram, model + 0xC08));
        auto joint_of = [&](int child) -> const Bridge::Joint* {
            for (const auto& jt : b.pose_joints) if (jt.child == child) return &jt;
            return nullptr;
        };
        auto local = [](const std::array<double, 3>& a) { return Vec3{float(a[0]), float(a[1]), float(a[2])}; };
        for (const auto& limb : {std::array<int, 3>{4, 6, 5}, {7, 9, 8}, {14, 13, 12}, {17, 16, 15}}) {
            const int u = limb[0], l = limb[1], e = limb[2];
            if (e >= nbones || !have[size_t(u)] || !have[size_t(l)] || !have[size_t(e)]) continue;
            const Bridge::Joint *j1 = joint_of(u), *j2 = joint_of(l), *j3 = joint_of(e);
            const int su = b.pose_source[size_t(u)], sl = b.pose_source[size_t(l)], se = b.pose_source[size_t(e)];
            if (!j1 || !j2 || !j3 || su < 0 || sl < 0 || se < 0) continue;
            const Vec3 h = m4_point(posed[size_t(u)], local(j1->child_local));
            const Vec3 kn = m4_point(posed[size_t(u)], local(j2->parent_local));
            const Vec3 an = m4_point(posed[size_t(l)], local(j3->parent_local));
            const float l1 = v_len(v_sub(kn, h)), l2 = v_len(v_sub(an, kn));
            auto s3p = [&](int bi) { const M4& m = s3w[size_t(bi)]; return Vec3{float(m[12]), float(m[13]), float(m[14])}; };
            Vec3 target = s3p(se);
            // Feet on the deck, not in it: THPS's foot is deeper below its ankle than Skate 3's, so when the foot
            // is over the board, lift the ankle until the sole clears the deck's top.
            if ((e == 5 || e == 8) && rig && size_t(e) < rig->vertices.size() && !rig->vertices[0].empty() && have[0]) {
                const M4& board = posed[0];
                Vec3 up{float(-board[4]), float(-board[5]), float(-board[6])};  // the board's model -y (PSX y is down)
                const float ul = v_len(up);
                if (ul > 1e-6f) {
                    up = v_mul(up, 1.0f / ul);
                    float deck_top = -1e9f;
                    Vec3 deck_c{0, 0, 0};
                    for (const Vec3& v : rig->vertices[0]) {
                        const Vec3 w = m4_point(board, v);
                        deck_top = std::max(deck_top, dot(up, w));
                        deck_c = v_add(deck_c, w);
                    }
                    deck_c = v_mul(deck_c, 1.0f / float(rig->vertices[0].size()));
                    // Sole depth below the ankle with the foot's own rotation.
                    const M4& fr = posed[size_t(e)];
                    const Vec3 ank = m4_point(fr, local(j3->child_local));
                    float sole = 1e9f;
                    for (const Vec3& v : rig->vertices[size_t(e)]) sole = std::min(sole, dot(up, v_sub(m4_point(fr, v), ank)));
                    const float need = deck_top - (dot(up, target) + sole);
                    const Vec3 off = v_sub(target, deck_c);
                    const float across = v_len(v_sub(off, v_mul(up, dot(off, up))));
                    const float k = float(character_scale(b));
                    if (need > 0.0f && need < 25.0f * k && across < 60.0f * k) target = v_add(target, v_mul(up, need));
                }
            }
            Vec3 to = v_sub(target, h);
            float d = v_len(to);
            if (d < 1e-3f || l1 < 1e-3f || l2 < 1e-3f) continue;
            const Vec3 n = v_mul(to, 1.0f / d);
            d = std::clamp(d, std::fabs(l1 - l2) + 0.5f, l1 + l2 - 0.05f);
            target = v_add(h, v_mul(n, d));
            // Bend direction: Skate 3's knee / elbow off the hip-ankle line (else the current one).
            Vec3 pole = v_sub(s3p(sl), s3p(su));
            pole = v_sub(pole, v_mul(n, dot(pole, n)));
            if (v_len(pole) < 1e-3f) {
                pole = v_sub(kn, h);
                pole = v_sub(pole, v_mul(n, dot(pole, n)));
            }
            if (v_len(pole) < 1e-3f) continue;
            pole = v_mul(pole, 1.0f / v_len(pole));
            const float x = (l1 * l1 - l2 * l2 + d * d) / (2 * d);
            const float y = std::sqrt(std::max(0.0f, l1 * l1 - x * x));
            const Vec3 knee = v_add(h, v_add(v_mul(n, x), v_mul(pole, y)));
            const M4 end_rot = posed[size_t(e)];
            const M4 r1 = rotate_onto(v_sub(kn, h), v_sub(knee, h), h);
            posed[size_t(u)] = m4_mul(r1, posed[size_t(u)]);
            posed[size_t(l)] = m4_mul(r1, posed[size_t(l)]);
            const Vec3 kn2 = m4_point(posed[size_t(l)], local(j2->child_local));
            const Vec3 an2 = m4_point(posed[size_t(l)], local(j3->parent_local));
            const M4 r2 = rotate_onto(v_sub(an2, kn2), v_sub(target, kn2), kn2);
            posed[size_t(l)] = m4_mul(r2, posed[size_t(l)]);
            // The hand / foot: its own (Skate 3) rotation, its side of the wrist / ankle on the forearm's / shin's.
            M4 w = end_rot;
            const Vec3 at = m4_point(posed[size_t(l)], local(j3->parent_local));
            const Vec3 c = local(j3->child_local);
            for (int r = 0; r < 3; r++)
                w[size_t(12 + r)] = at[size_t(r)] - (w[size_t(r)] * c[0] + w[size_t(4 + r)] * c[1] + w[size_t(8 + r)] * c[2]);
            posed[size_t(e)] = w;
        }
    }
    for (int i = 0; i < nbones; i++)
        if (have[size_t(i)]) n64_write(rdram, buffer + 64 * uint32_t(i), m4_mul(inv16, posed[size_t(i)]));
}
