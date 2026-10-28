// THPS level geometry -> skatecore world.
//
// tools/thps_level.py exports every level's triangles (THPS world units, PSX axes: y points down) to
// work/levels/<Level>.thl. This turns one into the in-memory .skate map skatecore's Sim loads.
//
// Frames: THPS (x, y down, z) maps to Skate 3's Y-up metres as (x, -y, -z) * scale. That is a 180 degree turn
// about X, a proper rotation, so triangle winding (and with it which side of a face is solid) is unchanged.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "s3/data/skate_map.hpp"

namespace thpskate {

// THPS world units per metre: Tony Hawk's model (hawk.psx, posed by the game) stands about 195 units from his
// feet to the top of his head, and he is 6'2" (1.88 m); the follow camera's 290-unit boom is then ~2.9 m.
inline constexpr float kUnitsPerMetre = 100.0f;

// THPS launches its skater straight up off any quarter pipe, and its levels are built for that: most quarter pipes
// top out at 50-56 degrees, with an invisible wall on the coping. Skate 3 keeps the speed along the ramp, which off
// a 56-degree top carries the skater out over the coping. Off ramps whose normal has under this much up in it (49
// degrees and steeper) the launch's speed toward the top is turned upward instead (Sim::set_thps_vert).
inline constexpr float kThpsVertMaxNormalY = 0.66f;

struct LevelGeometry {
    struct Tri {
        std::array<std::array<float, 3>, 3> v;  // THPS units, PSX axes
        uint32_t face = 0;                     // the mesh's per-triangle word (collision/trigger flags)
        uint32_t object = 0;                   // index of the level object the triangle came from
        // Face word flags, as THPS's segment test func_800837C8 reads them.
        bool ignored() const { return (face & 0x10000u) != 0; }                // no collision, no trigger
        bool trigger() const { return !ignored() && (face & 0x20000u) != 0; }  // crossing it runs a script
        bool solid() const { return (face & 0x30000u) == 0; }
    };
    std::vector<Tri> tris;
    std::vector<uint32_t> object_keys;  // per object: the TRG node key its trigger faces fire
    std::vector<std::vector<std::array<float, 3>>> rails;  // grind rails from the TRG (THPS units)
    struct Restart {
        std::array<float, 3> pos;  // THPS units
        uint32_t angle = 0;        // 4096ths of a turn about y; travel direction (-sin, 0, -cos)
    };
    std::vector<Restart> restarts;
};

bool load_thl(const std::string& path, LevelGeometry& out, std::string& error);

inline std::array<float, 3> to_s3(const std::array<float, 3>& p, float units_per_metre = kUnitsPerMetre) {
    return {p[0] / units_per_metre, -p[1] / units_per_metre, -p[2] / units_per_metre};
}
inline std::array<float, 3> from_s3(const std::array<float, 3>& p, float units_per_metre = kUnitsPerMetre) {
    return {p[0] * units_per_metre, -p[1] * units_per_metre, -p[2] * units_per_metre};
}

struct MapOptions {
    float units_per_metre = kUnitsPerMetre;
    bool two_sided = false;           // also add every triangle reversed
    bool edge_codes = true;           // per-edge corner codes from geometric adjacency (see level.cpp)
    std::array<float, 3> spawn{};     // Skate 3 metres (wheel-ground anchor)
    float heading = 0.0f;             // radians about +Y
};

// A collision-only .skate map (one portable material, no render data) from the level's solid faces, with the
// level's grind rails.
s3::data::SkateMap make_map(const LevelGeometry& level, const std::string& name, const MapOptions& options);

}  // namespace thpskate
