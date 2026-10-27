#include "level.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "s3/core/rs.hpp"

namespace thpskate {

bool load_thl(const std::string& path, LevelGeometry& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 8 || std::memcmp(b.data(), "THL4", 4) != 0) {
        error = path + " is not a THL4 level cache (re-run tools/thps_level.py)";
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, b.data() + 4, 4);
    constexpr size_t kRecord = 9 * 4 + 4 + 4;
    const size_t keys_at = 8 + size_t(count) * kRecord;
    if (b.size() < keys_at + 4) {
        error = path + ": truncated";
        return false;
    }
    out.tris.resize(count);
    const uint8_t* p = b.data() + 8;
    for (auto& t : out.tris) {
        float v[9];
        std::memcpy(v, p, sizeof(v));
        for (int k = 0; k < 3; k++) t.v[size_t(k)] = {v[3 * k], v[3 * k + 1], v[3 * k + 2]};
        std::memcpy(&t.face, p + 36, 4);
        std::memcpy(&t.object, p + 40, 4);
        p += kRecord;
    }
    uint32_t objects = 0;
    std::memcpy(&objects, b.data() + keys_at, 4);
    size_t at = keys_at + 4 + size_t(objects) * 4;
    if (b.size() < at + 4) {
        error = path + ": truncated";
        return false;
    }
    out.object_keys.resize(objects);
    std::memcpy(out.object_keys.data(), b.data() + keys_at + 4, size_t(objects) * 4);
    uint32_t nrails = 0;
    std::memcpy(&nrails, b.data() + at, 4);
    at += 4;
    out.rails.clear();
    for (uint32_t r = 0; r < nrails; r++) {
        uint32_t np = 0;
        if (at + 4 > b.size()) break;
        std::memcpy(&np, b.data() + at, 4);
        at += 4;
        std::vector<std::array<float, 3>> line(np);
        if (at + size_t(np) * 12 > b.size()) break;
        std::memcpy(line.data(), b.data() + at, size_t(np) * 12);
        at += size_t(np) * 12;
        out.rails.push_back(std::move(line));
    }
    uint32_t nrestarts = 0;
    if (at + 4 <= b.size()) {
        std::memcpy(&nrestarts, b.data() + at, 4);
        at += 4;
    }
    out.restarts.resize(nrestarts);
    for (auto& r : out.restarts) {
        if (at + 16 > b.size()) break;
        std::memcpy(r.pos.data(), b.data() + at, 12);
        std::memcpy(&r.angle, b.data() + at + 12, 4);
        at += 16;
    }
    if (at != b.size()) {
        error = path + ": size does not match its counts";
        return false;
    }
    return true;
}

namespace {

bool degenerate(const std::array<std::array<float, 3>, 3>& p) {
    const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
    const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    // skatecore rejects triangles it cannot normalise; 1e-10 m^2 is far below anything skateable.
    return !(nx * nx + ny * ny + nz * nz > 1e-10f);
}

// Skate 3's collision triangles carry per-edge codes (native_edges) saying which edges are real corners. Without
// them skatecore pairs edges only by exactly shared vertices, and every unpaired edge counts as a sharp convex
// corner whose contacts push the board sideways. THPS levels are built from separate objects whose faces meet
// without sharing vertices (and long floor edges run past short ramp edges), so unpaired edges lie all over
// otherwise smooth surfaces and catch the wheels. Here an edge's neighbour is any other face with a collinear,
// overlapping, opposite-running edge; codes then follow skatecore's own rule (convex unless concave or flat), and
// edges with no neighbour at all are treated as smooth rather than as corners.
void assign_edge_codes(std::vector<s3::data::Collision>& tris) {
    using V = std::array<float, 3>;
    auto sub = [](V a, V b) { return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](V a, V b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
    const size_t n = tris.size();
    std::vector<V> normal(n);
    for (size_t i = 0; i < n; i++) {
        V c = cross(sub(tris[i].points[1], tris[i].points[0]), sub(tris[i].points[2], tris[i].points[0]));
        const float l = std::sqrt(dot(c, c));
        normal[i] = {c[0] / l, c[1] / l, c[2] / l};
    }
    // Hash every edge into the 0.5 m cells its bounding box covers.
    constexpr float kCell = 0.5f, kTol = 0.02f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;  // cell -> edge ids (tri * 3 + edge)
    auto key = [](int x, int y, int z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(z) & 0x1FFFFF);
    };
    auto cells = [&](V a, V b, auto&& fn) {
        int lo[3], hi[3];
        for (int k = 0; k < 3; k++) {
            lo[k] = int(std::floor((std::min(a[k], b[k]) - kTol) / kCell));
            hi[k] = int(std::floor((std::max(a[k], b[k]) + kTol) / kCell));
        }
        for (int x = lo[0]; x <= hi[0]; x++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int z = lo[2]; z <= hi[2]; z++) fn(key(x, y, z));
    };
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t e = 0; e < 3; e++)
            cells(tris[i].points[e], tris[i].points[(e + 1) % 3], [&](uint64_t k) { grid[k].push_back(i * 3 + e); });

    std::vector<uint32_t> seen(n * 3, UINT32_MAX);
    std::vector<std::array<int, 3>> best_n(n, {-1, -1, -1});
    std::vector<std::array<float, 3>> best_c(n, {-2.0f, -2.0f, -2.0f});
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t e = 0; e < 3; e++) {
            const V a = tris[i].points[e], b = tris[i].points[(e + 1) % 3];
            const V d = sub(b, a);
            const float len = std::sqrt(dot(d, d));
            if (!(len > 1e-6f)) continue;
            const V u{d[0] / len, d[1] / len, d[2] / len};
            const uint32_t me = i * 3 + e;
            cells(a, b, [&](uint64_t k) {
                auto it = grid.find(k);
                if (it == grid.end()) return;
                for (uint32_t other : it->second) {
                    const uint32_t j = other / 3, f = other % 3;
                    if (j == i || seen[other] == me) continue;
                    seen[other] = me;
                    const V c0 = tris[j].points[f], c1 = tris[j].points[(f + 1) % 3];
                    if (dot(sub(c1, c0), u) >= 0.0f) continue;  // neighbours run the shared edge the other way
                    // both ends of the other edge within kTol of this edge's line
                    auto off = [&](V p) { V w = sub(p, a); const float t = dot(w, u); V q{w[0] - u[0] * t, w[1] - u[1] * t, w[2] - u[2] * t}; return std::sqrt(dot(q, q)); };
                    if (off(c0) > kTol || off(c1) > kTol) continue;
                    const float t0 = dot(sub(c0, a), u), t1 = dot(sub(c1, a), u);
                    const float overlap = std::min(len, std::max(t0, t1)) - std::max(0.0f, std::min(t0, t1));
                    const float other_len = std::abs(t1 - t0);
                    if (overlap < 0.5f * std::min(len, other_len) || overlap < 0.01f) continue;
                    const float cs = std::max(-1.0f, std::min(1.0f, dot(normal[i], normal[j])));
                    if (cs > best_c[i][e]) {
                        best_c[i][e] = cs;
                        best_n[i][e] = int(j);
                    }
                }
            });
        }
    }
    const float pi2 = s3::rs::from_bits(0x411de9e7u);  // the native code's cosine table: 1 - pi^2 / (8 << code)
    auto code_for = [&](float cs) {
        int best = 27;
        float err = 1e9f;
        for (int c = 0; c < 28; c++) {
            const float v = 1.0f - pi2 / float(8u << c);
            if (std::abs(v - cs) < err) err = std::abs(v - cs), best = c;
        }
        return uint8_t(best);
    };
    for (uint32_t i = 0; i < n; i++) {
        std::array<bool, 3> convex{};
        std::array<uint8_t, 3> codes{};
        for (int e = 0; e < 3; e++) {
            const int j = best_n[i][e];
            if (j < 0) {  // open edge: smooth, never a catching corner
                codes[e] = code_for(1.0f);
                continue;
            }
            const float cs = best_c[i][e];
            const V a = tris[i].points[size_t(e)], b = tris[i].points[size_t((e + 1) % 3)];
            const float orientation = dot(sub(b, a), cross(normal[i], normal[size_t(j)]));
            convex[size_t(e)] = !(orientation <= -1.0e-6f || cs >= 0.9999f);
            codes[e] = uint8_t(code_for(cs) | (convex[size_t(e)] ? 0x20 : 0));
        }
        // a corner whose two edges are both smooth or concave gives face-normal contacts, as for flat fans
        for (int v = 0; v < 3; v++)
            if (!convex[size_t(v)] && !convex[size_t((v + 2) % 3)]) codes[v] |= 0x40;
        tris[i].native_edges = codes;
    }
}

}  // namespace

s3::data::SkateMap make_map(const LevelGeometry& level, const std::string& name, const MapOptions& options) {
    s3::data::SkateMap map;
    map.version = 15;
    map.name = name;
    map.spawn = options.spawn;
    map.heading = options.heading;
    s3::data::Material concrete;
    concrete.name = "thps";
    concrete.friction = 0.6f;
    concrete.color = {0.5f, 0.5f, 0.5f};
    map.materials.push_back(concrete);
    const uint32_t surface = concrete.audio | (concrete.physics << 7) | (concrete.pattern << 12);
    for (const auto& t : level.tris) {
        if (!t.solid()) continue;  // trigger planes and decoration: THPS's skater passes through them too
        s3::data::Collision c;
        for (int k = 0; k < 3; k++) c.points[k] = to_s3(t.v[k], options.units_per_metre);
        if (degenerate(c.points)) continue;
        c.surface = surface;
        c.material = 1;
        map.geometry.collision.push_back(c);
        if (options.two_sided) {
            std::swap(c.points[1], c.points[2]);
            map.geometry.collision.push_back(c);
        }
    }
    if (options.edge_codes) assign_edge_codes(map.geometry.collision);
    for (size_t r = 0; r < level.rails.size(); r++) {
        s3::data::Rail rail;
        rail.name = "thps_rail_" + std::to_string(r);
        rail.closed = false;
        for (const auto& p : level.rails[r]) rail.points.push_back(to_s3(p, options.units_per_metre));
        if (rail.points.size() >= 2) map.rails.push_back(std::move(rail));
    }
    return map;
}

}  // namespace thpskate
