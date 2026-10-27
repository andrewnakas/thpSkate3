// Headless check that skatecore skates on a THPS level: tools/native/skate_on_thps CORE.PACK LEVEL.thl X Y Z [heading]
// X Y Z: a THPS world position (units, PSX axes) to spawn at. Pushes for 4 s, ollies, then rolls; prints the
// deck position every 20 ticks (in THPS units) with the physical state and any trick or score names.
// Env: OLLIE_T (tick of the ollie, default 300), STEER (left stick x held from the ollie on, -32767..32767),
// POP (trainer pop multiplier).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "s3/data/vfs.hpp"
#include "s3/game/driver.hpp"
#include "skate/level.hpp"

using s3::game::driver::Pad;
using s3::game::driver::Sim;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: skate_on_thps CORE.PACK LEVEL.thl X Y Z [heading]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto vfs = s3::data::Vfs::from_pack(std::move(pack));
    if (!vfs.ok) { std::fprintf(stderr, "pack: %s\n", vfs.error.c_str()); return 1; }

    thpskate::LevelGeometry level;
    std::string err;
    if (!thpskate::load_thl(argv[2], level, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    thpskate::MapOptions opt;
    opt.spawn = thpskate::to_s3({std::strtof(argv[3], nullptr), std::strtof(argv[4], nullptr), std::strtof(argv[5], nullptr)});
    opt.heading = argc > 6 ? std::strtof(argv[6], nullptr) : 0.0f;
    auto map = thpskate::make_map(level, "thps", opt);
    std::fprintf(stderr, "map: %zu collision triangles, spawn %.2f %.2f %.2f m\n", map.geometry.collision.size(),
                 opt.spawn[0], opt.spawn[1], opt.spawn[2]);
    auto loaded = Sim::load(vfs.value, "assets", &map, nullptr);
    if (loaded.is_err()) { std::fprintf(stderr, "sim: %s\n", loaded.unwrap_err().c_str()); return 1; }
    auto sim = std::move(loaded.unwrap());

    std::printf("rails: %zu\n", level.rails.size());
    const int ollie = std::getenv("OLLIE_T") ? std::atoi(std::getenv("OLLIE_T")) : 300;
    const int steer = std::getenv("STEER") ? std::atoi(std::getenv("STEER")) : 0;
    if (std::getenv("POP")) {
        s3::game::trainer::TrainerTuning tt;
        tt.pop = std::strtof(std::getenv("POP"), nullptr);
        sim->set_tuning(tt, 1.0f);
    }
    const int ticks = std::getenv("TICKS") ? std::atoi(std::getenv("TICKS")) : 600;
    for (int t = 0; t < ticks; t++) {
        Pad pad{};
        const bool push_phase = t >= 30 && t < std::min(270, ollie - 30);
        if (t >= ollie - 20 && t < ollie + 60) pad.lx = int16_t(steer);
        if (push_phase && (t / 30) % 2 == 0) pad.buttons = 0x1000;  // A: push
        if (t >= ollie && t < ollie + 12) pad.ry = -32767;                   // right stick down...
        if (t >= ollie + 12 && t < ollie + 18) pad.ry = 32767;                    // ...then up: ollie
        auto step = sim->step(pad);
        if (step.is_err()) { std::fprintf(stderr, "tick %d: %s\n", t, step.unwrap_err().c_str()); return 1; }
        const auto& r = step.unwrap();
        const auto& deck = r.board.back().position;
        const auto p = thpskate::from_s3({deck[0], deck[1], deck[2]});
        const auto& v = r.board.back().linear_velocity;
        if (t % 20 == 0 || !r.trick_name.empty())
            std::printf("t%3d  thps(%7.0f %6.0f %7.0f)  speed %5.2f m/s  %-14s grounded=%d contacts=%u %s %s\n", t,
                        p[0], p[1], p[2], std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), r.physical_state.c_str(),
                        r.grounded, r.contact_count, r.trick_name.c_str(), r.score_name.c_str());
    }
    return 0;
}
