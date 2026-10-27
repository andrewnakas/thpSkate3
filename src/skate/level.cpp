#include "level.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

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
