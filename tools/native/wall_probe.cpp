// Invisible-wall hunt: tools/native/wall_probe CORE.PACK LEVEL.thl [MAX_PROBES]
// For every ramp patch (solid faces sloped 30-70 degrees, one probe per 3 m cell and facing), drops the skater on the
// floor 7 m in front of it facing straight up the slope, pushes hard (push speed/power x3) and watches the run. A
// "wall" is the board losing over 55% of its speed within 3 ticks, or Skate 3 starting a wallride or wipeout, while
// it is on or near the ramp. Prints one line per hit with the THPS position (units) and what happened.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
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

struct Tri { V p[3]; V n; };

// Distance from q to triangle t (Ericson, closest point on triangle).
static float tri_dist(const Tri& t, V q) {
    const V a = t.p[0], b = t.p[1], c = t.p[2];
    const V ab = sub(b, a), ac = sub(c, a), ap = sub(q, a);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    auto len = [&](V x) { return std::sqrt(dot(x, x)); };
    auto at = [&](float u, float v) { return V{a[0] + ab[0] * u + ac[0] * v, a[1] + ab[1] * u + ac[1] * v, a[2] + ab[2] * u + ac[2] * v}; };
    if (d1 <= 0 && d2 <= 0) return len(ap);
    const V bp = sub(q, b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return len(bp);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return len(sub(q, at(d1 / (d1 - d3), 0)));
    const V cp = sub(q, c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return len(cp);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return len(sub(q, at(0, d2 / (d2 - d6))));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return len(sub(q, V{b[0] + (c[0] - b[0]) * w, b[1] + (c[1] - b[1]) * w, b[2] + (c[2] - b[2]) * w}));
    }
    const float den = 1.0f / (va + vb + vc);
    return len(sub(q, at(vb * den, vc * den)));
}

static std::vector<Tri> solid_tris(const thpskate::LevelGeometry& level) {
    std::vector<Tri> tris;
    for (const auto& t : level.tris) {
        if (!t.solid()) continue;
        Tri s;
        for (int k = 0; k < 3; k++) s.p[k] = thpskate::to_s3(t.v[k]);
        V n = cross(sub(s.p[1], s.p[0]), sub(s.p[2], s.p[0]));
        const float l = std::sqrt(dot(n, n));
        if (!(l > 1e-6f)) continue;
        s.n = {n[0] / l, n[1] / l, n[2] / l};
        tris.push_back(s);
    }
    return tris;
}

// Highest floor (normal y > 0.85) hit by a ray straight down from o, if any.
static std::optional<float> floor_below(const std::vector<Tri>& tris, V o) {
    std::optional<float> best;
    for (const auto& t : tris) {
        if (t.n[1] < 0.85f) continue;
        const float d = dot(t.n, sub(t.p[0], o)) / t.n[1];  // distance down along -y is -d... solve plane at x,z
        const float y = o[1] + d;
        if (y > o[1]) continue;
        const V q{o[0], y, o[2]};
        bool inside = true;
        for (int k = 0; k < 3 && inside; k++)
            inside = dot(cross(sub(t.p[(k + 1) % 3], t.p[k]), sub(q, t.p[k])), t.n) >= -1e-6f;
        if (inside && (!best || y > *best)) best = y;
    }
    return best;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: wall_probe CORE.PACK LEVEL.thl [MAX_PROBES]\n");
        return 2;
    }
    const int max_probes = argc > 3 ? std::atoi(argv[3]) : 400;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto vfs = s3::data::Vfs::from_pack(std::move(pack));
    if (!vfs.ok) { std::fprintf(stderr, "pack: %s\n", vfs.error.c_str()); return 1; }
    thpskate::LevelGeometry level;
    std::string err;
    if (!thpskate::load_thl(argv[2], level, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

    std::vector<Tri> tris = solid_tris(level);
    // Optional reference (REF=LEVEL.thl, THPS's own unsmoothed faces): count ticks the wheels are down where THPS
    // has no surface within 0.6 m of the deck, i.e. the skater riding on something that is not there in THPS.
    std::vector<Tri> ref;
    if (const char* rp = std::getenv("REF")) {
        thpskate::LevelGeometry rl;
        if (thpskate::load_thl(rp, rl, err)) ref = solid_tris(rl);
    }
    int phantom_total = 0;

    // Ramp patches -> probes (spawn on the floor 7 m out, heading up the slope).
    struct Probe { V spawn; float heading; V ramp; };
    std::vector<Probe> probes;
    std::map<std::tuple<int, int, int, int>, bool> seen;
    for (const auto& t : tris) {
        if (t.n[1] < 0.34f || t.n[1] > 0.87f) continue;
        const V c{(t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3, (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3,
                  (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3};
        const float h = std::hypot(t.n[0], t.n[2]);
        const V up{-t.n[0] / h, 0, -t.n[2] / h};  // horizontal uphill direction
        const int dir = int(std::lround(std::atan2(up[0], up[2]) / (3.14159265f / 4))) & 7;
        const auto key = std::make_tuple(int(std::floor(c[0] / 3)), int(std::floor(c[1] / 3)), int(std::floor(c[2] / 3)), dir);
        if (seen.count(key)) continue;
        seen[key] = true;
        const V start{c[0] - up[0] * 7, c[1] + 0.3f, c[2] - up[2] * 7};
        const auto fy = floor_below(tris, start);
        if (!fy || *fy < c[1] - 4.0f) continue;
        probes.push_back({{start[0], *fy, start[2]}, std::atan2(up[0], up[2]), c});
        if (int(probes.size()) >= max_probes) break;
    }
    std::printf("%s: %zu ramp probes\n", argv[2], probes.size());

    s3::game::trainer::TrainerTuning tt;
    tt.push_speed = 3.0f;
    tt.push_power = 3.0f;
    int hits = 0;
    for (size_t i = 0; i < probes.size(); i++) {
        const auto& pr = probes[i];
        thpskate::MapOptions opt;
        opt.spawn = pr.spawn;
        opt.heading = pr.heading;
        auto map = thpskate::make_map(level, "thps", opt);
        auto loaded = Sim::load(vfs.value, "assets", &map, nullptr);
        if (loaded.is_err()) continue;
        auto sim = std::move(loaded.unwrap());
        sim->set_tuning(tt, 1.0f);
        std::vector<float> speed;
        std::string event;
        float rise_grounded = 0.0f, rise_air = 0.0f;
        std::map<std::string, int> states;
        V where{};
        for (int t = 0; t < 360; t++) {
            Pad pad{};
            if (t >= 20 && t < 200 && (t / 20) % 2 == 0) pad.buttons = 0x1000;  // A: push
            auto step = sim->step(pad);
            if (step.is_err()) { event = "step error"; break; }
            const auto& r = step.unwrap();
            const auto& b = r.board.back();
            const auto& v = b.linear_velocity;
            speed.push_back(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            const V p{b.position[0], b.position[1], b.position[2]};
            if (std::getenv("VERBOSE")) states[r.physical_state]++;
            const float rise = p[1] - pr.spawn[1];
            if (const char* tr = std::getenv("TRACE"); tr && std::atoi(tr) == int(i) && t % 6 == 0) {
                const V q = thpskate::from_s3(p);
                std::printf("  t%3d thps(%6.0f %6.0f %6.0f) rise %5.2f speed %5.2f vy %5.2f grounded=%d contacts=%u %s\n", t, q[0], q[1],
                            q[2], rise, speed.back(), v[1], r.grounded, r.contact_count, r.physical_state.c_str());
            }
            if (r.grounded && !r.wiping_out) rise_grounded = std::max(rise_grounded, rise);
            else rise_air = std::max(rise_air, rise);
            if (!ref.empty() && r.grounded && !r.wiping_out && r.contact_count > 0) {
                float best = 1e9f;
                for (const auto& rt : ref) {
                    const float dx = std::max({rt.p[0][0], rt.p[1][0], rt.p[2][0]}) + 0.6f;
                    const float nx = std::min({rt.p[0][0], rt.p[1][0], rt.p[2][0]}) - 0.6f;
                    const float dz = std::max({rt.p[0][2], rt.p[1][2], rt.p[2][2]}) + 0.6f;
                    const float nz = std::min({rt.p[0][2], rt.p[1][2], rt.p[2][2]}) - 0.6f;
                    if (p[0] < nx || p[0] > dx || p[2] < nz || p[2] > dz) continue;
                    best = std::min(best, tri_dist(rt, p));
                }
                if (best > 0.6f) {
                    phantom_total++;
                    if (event.empty()) {
                        event = "phantom surface (" + std::to_string(best).substr(0, 4) + " m from THPS)";
                        where = p;
                    }
                }
            }
            const float near = std::hypot(p[0] - pr.ramp[0], p[2] - pr.ramp[2]);
            const bool on_ramp = near < 6.0f && p[1] > pr.spawn[1] + 0.25f;
            const size_t n = speed.size();
            if (!event.empty()) {
            } else if (on_ramp && n > 3 && speed[n - 4] > 3.0f && speed[n - 1] < 0.45f * speed[n - 4])
                event = "stopped " + std::to_string(speed[n - 4]).substr(0, 4) + "->" + std::to_string(speed[n - 1]).substr(0, 4) + " m/s";
            else if (on_ramp && (r.physical_state.find("Wall") != std::string::npos || r.wiping_out))
                event = r.wiping_out ? "wipeout" : r.physical_state;
            if (!event.empty() && where[0] == 0 && where[1] == 0 && where[2] == 0) where = p;
        }
        // Riding up past a ramp's top on a surface instead of launching: wheels on something 3.5 m+ up.
        if (std::getenv("VERBOSE")) {
            std::printf("probe %zu ramp (%.1f %.1f %.1f) rise ground %.2f air %.2f :", i, pr.ramp[0], pr.ramp[1], pr.ramp[2], rise_grounded, rise_air);
            for (auto& [k, v] : states) std::printf(" %s=%d", k.c_str(), v);
            std::printf("\n");
        }
        if (event.empty()) continue;
        hits++;
        const V s = thpskate::from_s3(pr.spawn), w = thpskate::from_s3(where), c = thpskate::from_s3(pr.ramp);
        std::printf("HIT %-28s at (%.0f %.0f %.0f)  ramp (%.0f %.0f %.0f)  spawn %.0f,%.0f,%.0f,%.3f  maxspeed %.1f\n",
                    event.c_str(), w[0], w[1], w[2], c[0], c[1], c[2], s[0], s[1], s[2], pr.heading,
                    *std::max_element(speed.begin(), speed.end()));
        std::fflush(stdout);
    }
    std::printf("%s: %d hits of %zu probes, %d phantom-surface ticks\n", argv[2], hits, probes.size(), phantom_total);
    return 0;
}
