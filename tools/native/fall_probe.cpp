// Fall-through hunt: tools/native/fall_probe CORE.PACK LEVEL.thl [RUNS] [UNITS_PER_METRE]
// Drops the skater on floors all over the level (one run per 6 m cell, random heading), pushes and ollies for ten
// seconds, and reports every tick where the board passes down through the front of a solid floor-ish face (normal
// y > 0.2), i.e. falls through the map, with the face it went through and the speed it had.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "s3/data/vfs.hpp"
#include "s3/game/driver.hpp"
#include "skate/level.hpp"

using s3::game::driver::Pad;
using s3::game::driver::Sim;
using V = std::array<float, 3>;

static V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
static V cross(V a, V b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
static float dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Tri { V p[3]; V n; V lo, hi; };

static std::vector<Tri> solid_tris(const thpskate::LevelGeometry& level, float upm) {
    std::vector<Tri> tris;
    for (const auto& t : level.tris) {
        if (!t.solid()) continue;
        Tri s;
        for (int k = 0; k < 3; k++) s.p[k] = thpskate::to_s3(t.v[k], upm);
        V n = cross(sub(s.p[1], s.p[0]), sub(s.p[2], s.p[0]));
        const float l = std::sqrt(dot(n, n));
        if (!(l > 1e-6f)) continue;
        s.n = {n[0] / l, n[1] / l, n[2] / l};
        for (int k = 0; k < 3; k++) {
            s.lo[k] = std::min({s.p[0][k], s.p[1][k], s.p[2][k]});
            s.hi[k] = std::max({s.p[0][k], s.p[1][k], s.p[2][k]});
        }
        tris.push_back(s);
    }
    return tris;
}

static bool inside(const Tri& t, V q) {
    for (int k = 0; k < 3; k++)
        if (dot(cross(sub(t.p[(k + 1) % 3], t.p[k]), sub(q, t.p[k])), t.n) < -1e-6f) return false;
    return true;
}

// Highest floor (normal y > 0.85) hit by a ray straight down from o.
static std::optional<float> floor_below(const std::vector<Tri>& tris, V o) {
    std::optional<float> best;
    for (const auto& t : tris) {
        if (t.n[1] < 0.85f || o[0] < t.lo[0] || o[0] > t.hi[0] || o[2] < t.lo[2] || o[2] > t.hi[2]) continue;
        const float y = o[1] + dot(t.n, sub(t.p[0], o)) / t.n[1];
        if (y > o[1]) continue;
        if (inside(t, {o[0], y, o[2]}) && (!best || y > *best)) best = y;
    }
    return best;
}

// Any solid face straight below o (for the out-of-world test: slopes count too).
static bool below_any(const std::vector<Tri>& tris, V o) {
    for (const auto& t : tris) {
        if (std::fabs(t.n[1]) < 0.05f || o[0] < t.lo[0] || o[0] > t.hi[0] || o[2] < t.lo[2] || o[2] > t.hi[2]) continue;
        const float y = o[1] + dot(t.n, sub(t.p[0], o)) / t.n[1];
        if (y <= o[1] && inside(t, {o[0], y, o[2]})) return true;
    }
    return false;
}

// The face the segment a->b crosses from its front to its back (a above, b below), if any.
static const Tri* crossed(const std::vector<Tri>& tris, V a, V b) {
    for (const auto& t : tris) {
        if (t.n[1] < 0.2f) continue;
        bool out = false;
        for (int k = 0; k < 3 && !out; k++) out = std::max(a[k], b[k]) < t.lo[k] || std::min(a[k], b[k]) > t.hi[k];
        if (out) continue;
        const float da = dot(t.n, sub(a, t.p[0])), db = dot(t.n, sub(b, t.p[0]));
        if (!(da > 0.0f && db < -0.05f)) continue;  // 5 cm past the back: wheels sinking into a face is not a fall
        const float u = da / (da - db);
        const V q{a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u};
        if (inside(t, q)) return &t;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: fall_probe CORE.PACK LEVEL.thl [RUNS] [UNITS_PER_METRE]\n");
        return 2;
    }
    const int max_runs = argc > 3 ? std::atoi(argv[3]) : 300;
    const float upm = argc > 4 ? std::strtof(argv[4], nullptr) : thpskate::kUnitsPerMetre;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto vfs = s3::data::Vfs::from_pack(std::move(pack));
    if (!vfs.ok) { std::fprintf(stderr, "pack: %s\n", vfs.error.c_str()); return 1; }
    thpskate::LevelGeometry level;
    std::string err;
    if (!thpskate::load_thl(argv[2], level, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const std::vector<Tri> tris = solid_tris(level, upm);

    // Spawns: the top floor under the centre of every 6 m cell that has one.
    std::map<std::pair<int, int>, V> cells;
    for (const auto& t : tris) {
        if (t.n[1] < 0.95f) continue;
        const V c{(t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3, (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3,
                  (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3};
        const auto key = std::make_pair(int(std::floor(c[0] / 6)), int(std::floor(c[2] / 6)));
        if (!cells.count(key) || c[1] > cells[key][1]) cells[key] = c;
    }
    std::vector<V> spawns;
    for (auto& [k, c] : cells)
        if (auto fy = floor_below(tris, {c[0], c[1] + 0.5f, c[2]})) spawns.push_back({c[0], *fy, c[2]});
    std::mt19937 rng(argc > 5 ? std::atoi(argv[5]) : 1);
    std::shuffle(spawns.begin(), spawns.end(), rng);
    if (int(spawns.size()) > max_runs) spawns.resize(max_runs);
    std::printf("%s: %zu runs, %zu solid faces\n", argv[2], spawns.size(), tris.size());

    s3::game::trainer::TrainerTuning tt;
    tt.push_speed = std::getenv("PUSH") ? std::strtof(std::getenv("PUSH"), nullptr) : 2.09f;
    tt.push_power = tt.push_speed;
    tt.pop = std::getenv("POP") ? std::strtof(std::getenv("POP"), nullptr) : 1.5f;
    int falls = 0;
    std::uniform_real_distribution<float> ang(-3.14159265f, 3.14159265f);
    for (size_t i = 0; i < spawns.size(); i++) {
        thpskate::MapOptions opt;
        opt.units_per_metre = upm;
        opt.spawn = spawns[i];
        opt.heading = ang(rng);
        auto map = thpskate::make_map(level, "thps", opt);
        auto loaded = Sim::load(vfs.value, "assets", &map, nullptr);
        if (loaded.is_err()) continue;
        auto sim = std::move(loaded.unwrap());
        sim->set_tuning(tt, 1.0f);
        std::optional<V> prev;
        V vprev{};
        std::string sprev;
        for (int t = 0; t < 600; t++) {
            Pad pad{};
            const int ph = t % 60;
            if (ph < 20) pad.buttons = 0x1000;                       // A: push
            else if (ph >= 40 && ph < 46) pad.ry = (i & 1) ? 32000 : -32000;  // flick: back...
            else if (ph >= 46 && ph < 50) pad.ry = (i & 1) ? -32000 : 32000;  // ...then forward = ollie
            pad.lx = int16_t(std::sin(t * 0.03f + i) * 20000);
            auto step = sim->step(pad);
            if (step.is_err()) break;
            const auto& r = step.unwrap();
            const auto& b = r.board.back();
            const V p{b.position[0], b.position[1], b.position[2]};
            const V& v = b.linear_velocity;
            // Teleports (Skate 3's reset after a wipeout puts the board where the rider is) are not motion.
            const bool jump = prev && dot(sub(p, *prev), sub(p, *prev)) > 1.5f * 1.5f;
            // Out of the world: nothing at all below the board (checked twice a second).
            if (t % 30 == 0 && !r.wiping_out && !floor_below(tris, {p[0], p[1] + 0.5f, p[2]}) &&
                !below_any(tris, {p[0], p[1] + 0.5f, p[2]})) {
                const V w = thpskate::from_s3(p, upm), s = thpskate::from_s3(spawns[i], upm);
                std::printf("VOID run %zu t%d at thps(%.0f %.0f %.0f) %s grounded=%d  spawn %.0f,%.0f,%.0f,%.3f\n", i, t, w[0], w[1],
                            w[2], r.physical_state.c_str(), r.grounded, s[0], s[1], s[2], opt.heading);
                falls++;
                break;
            }
            if (prev && !jump) {
                if (const Tri* ct = crossed(tris, *prev, p)) {
                    const V w = thpskate::from_s3(p, upm), s = thpskate::from_s3(spawns[i], upm);
                    const V a = thpskate::from_s3(ct->p[0], upm), bb = thpskate::from_s3(ct->p[1], upm), c = thpskate::from_s3(ct->p[2], upm);
                    std::printf("FALL run %zu t%d at thps(%.0f %.0f %.0f) v(%.1f %.1f %.1f) %s->%s grounded=%d through n(%.2f %.2f %.2f) "
                                "tri (%.0f %.0f %.0f)(%.0f %.0f %.0f)(%.0f %.0f %.0f)  spawn %.0f,%.0f,%.0f,%.3f\n",
                                i, t, w[0], w[1], w[2], v[0], v[1], v[2], sprev.c_str(), r.physical_state.c_str(), r.grounded, ct->n[0],
                                ct->n[1], ct->n[2], a[0], a[1], a[2], bb[0], bb[1], bb[2], c[0], c[1], c[2], s[0], s[1], s[2], opt.heading);
                    std::fflush(stdout);
                    falls++;
                    const V w0 = thpskate::from_s3(*prev, upm);
                    std::printf("    from thps(%.0f %.0f %.0f), moved %.2f m in one tick\n", w0[0], w0[1], w0[2],
                                std::sqrt(dot(sub(p, *prev), sub(p, *prev))));
                    if (std::getenv("AFTER")) {
                        for (int k = 1; k <= 120; k++) {
                            auto st = sim->step(Pad{});
                            if (st.is_err()) break;
                            const auto& bk = st.unwrap().board.back();
                            if (k % 15 == 0) {
                                const V q = thpskate::from_s3({bk.position[0], bk.position[1], bk.position[2]}, upm);
                                std::printf("    +%d thps(%.0f %.0f %.0f) %s grounded=%d\n", k, q[0], q[1], q[2],
                                            st.unwrap().physical_state.c_str(), st.unwrap().grounded);
                            }
                        }
                    }
                    break;
                }
            }
            prev = p;
            vprev = {v[0], v[1], v[2]};
            sprev = r.physical_state;
        }
    }
    std::printf("%s: %d falls in %zu runs\n", argv[2], falls, spawns.size());
    return 0;
}
