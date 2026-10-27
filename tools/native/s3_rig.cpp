// Dump skatecore's Skate 3 rig: bone index, name, parent, and global bone positions (animation space, metres)
// after 60 idle ticks and while pushing. tools/native/s3_rig CORE.PACK LEVEL.thl X Y Z HEADING
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "s3/data/animation_banks.hpp"
#include "s3/data/vfs.hpp"
#include "s3/game/driver.hpp"
#include "skate/level.hpp"

using namespace s3::data;
using s3::game::driver::Pad;
using s3::game::driver::Sim;

int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto vfs = Vfs::from_pack(std::move(pack));
    auto banks = AnimationBanks::load(vfs.value, "assets");
    if (!banks.ok) { std::fprintf(stderr, "banks: %s\n", banks.error.c_str()); return 1; }
    const abin::Hierarchy* h = banks->banks.at(0)->hierarchy();
    thpskate::LevelGeometry level;
    std::string err;
    thpskate::load_thl(argv[2], level, err);
    thpskate::MapOptions opt;
    opt.spawn = thpskate::to_s3({std::strtof(argv[3], nullptr), std::strtof(argv[4], nullptr), std::strtof(argv[5], nullptr)});
    opt.heading = std::strtof(argv[6], nullptr);
    auto map = thpskate::make_map(level, "thps", opt);
    auto sim = std::move(Sim::load(vfs.value, "assets", &map, nullptr).unwrap());
    auto dump = [&](const char* label) {
        const auto p = sim->presentation();
        std::printf("== %s  root t=(%.3f %.3f %.3f)\n", label, p.root[3][0], p.root[3][1], p.root[3][2]);
        for (size_t i = 0; i < p.bones->size(); i++) {
            const auto& m = (*p.bones)[i];
            std::printf("%2zu %-22s parent %2d  pos (%7.3f %7.3f %7.3f)  x(%5.2f %5.2f %5.2f) y(%5.2f %5.2f %5.2f) z(%5.2f %5.2f %5.2f)\n",
                        i, i < h->bone_names.size() ? h->bone_names[i].c_str() : "?",
                        i < h->parents.size() ? h->parents[i] : -9, m[3][0], m[3][1], m[3][2],
                        m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
        }
    };
    for (int t = 0; t < 60; t++) sim->step(Pad{});
    dump("idle");
    for (int t = 0; t < 40; t++) { Pad p{}; p.buttons = 0x1000; sim->step(p); }
    dump("pushing");
    return 0;
}
