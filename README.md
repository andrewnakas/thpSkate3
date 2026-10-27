# thpSkate3

Tony Hawk's Pro Skater (N64) with Skate 3's skating. THPS keeps its levels, goals, timer, HUD, scoring, combos,
gaps, tapes and characters; [skatecore-cpp](https://github.com/andrewnakas/skatecore-cpp) (Skate 3's skateboarding
runtime) replaces the skater: flick-it tricks, Skate 3 physics, grinds, manuals, bails, ragdoll wipeouts and the
Skate 3 camera. The THPS characters are animated by Skate 3's skeleton. An SK8TRAINER-style menu tunes it live.

It runs on the native port of the [THPS64 decompilation](https://github.com/andrewnakas/thps64) (N64Recomp +
N64ModernRuntime + RT64). Nothing from either game is in this repository: you need your own THPS N64 ROM (US V1.1)
and the Skate 3 cleanroom pack.

## How it works

| Piece | Where |
|---|---|
| Hooks injected into recompiled THPS code (skater physics, camera, bone matrices) | `recomp/thps.toml`, `recomp/include/skate_hooks.h` |
| Bridge: runs skatecore each frame, writes the skater, scores tricks through THPS's combo code, fires THPS trigger scripts (glass, gaps), poses the THPS character from Skate 3's skeleton | `port/src/skate_bridge.cpp` |
| Skater sounds: THPS's own rolling/grind/air loops (by surface) and ollie, push, landing and bail sounds, driven by Skate 3's state and events | `port/src/skate_bridge.cpp` |
| Trainer menu (RT64 ImGui overlay) | `port/src/skate_trainer.cpp` |
| Level collision, grind rails and restart points extracted from the ROM, with collision cleanup that keeps every surface where THPS draws it (seams welded, T-junctions repaired, floor mats
and plates flattened so wheels roll over them, walls standing on a quarter pipe's lip made non-solid above a 30 cm
vert so Skate 3 launches straight up off the coping instead of riding up the wall, and per-edge corner codes so
seams between THPS's pieces don't catch the wheels) | `tools/thps_level.py`, `src/skate/level.cpp` |
| Character rigs (bones, joints) for the retargeting | `tools/thps_skel.py` |
| ROM asset pack reader | `tools/thps_pack.py` |

THPS facts the bridge relies on (scale ~100 units per metre, face flags, trigger brackets, the rig) are noted in the
source comments.

## Setup (Linux)

Needs clang (tested with clang 20), cmake, ninja, Python 3 (with Pillow for the test tools), SDL2 2.32 development
files, a Vulkan GPU, and a checkout of the [THPS64 decomp](https://github.com/andrewnakas/thps64) (for its ERZ
unpacker and `work/main.bin`; `THPS64=/path/to/thps64`, default `~/thps64-claude`).

```sh
# dependencies (cloned into deps/ at pinned revisions, not committed) + thpSkate3's patches from patches/
tools/setup_deps.sh
# (SDL2 2.32 without system dev packages: build it into deps/prefix)

# the game's code image, unpacked by the decomp from your ROM
mkdir -p work
cp "/path/to/Tony Hawk's Pro Skater (U) (V1.1).z64" work/thps.z64
python3 "$THPS64/tools/build_main.py" work/thps.z64 work/   # -> work/main.bin, work/boot.bin

# recompile the game from your ROM
python3 tools/make_recomp_rom.py work/thps.z64
(cd deps/pw64/lib/N64ModernRuntime/N64Recomp && cmake -B build -G Ninja && ninja -C build N64Recomp RSPRecomp)
python3 tools/recomp_loop.py deps/pw64/lib/N64ModernRuntime/N64Recomp/build/N64Recomp
(cd recomp && ../deps/pw64/lib/N64ModernRuntime/N64Recomp/build/RSPRecomp aspMain.thps.toml)
N64MR=deps/pw64/lib/N64ModernRuntime python3 tools/gen_reimplemented_decls.py

# extract levels and rigs from the ROM
python3 tools/thps_pack.py work/thps.z64 work/pack
python3 -c "import sys; sys.path.insert(0,'tools'); import thps_level as l; l.export_all('work/pack','work/levels')"
python3 -c "import sys; sys.path.insert(0,'tools'); import thps_skel as s; s.export_rigs('work/pack','work/levels/rigs.thr')"

# Skate 3 cleanroom pack (never commit it): core.pack from the cleanroom site's pack.json
# (https://andrewnakas.github.io/skate3-cleanroom/, as skatecore-cpp's README describes) -> data/cleanpack/core.pack

# build and run
cd port
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DTHPS_WITH_RUNTIME=ON \
  -DTHPS_WITH_RECOMPILED=ON -DTHPS_LIB_DIR=$PWD/../deps/pw64/lib -DCMAKE_PREFIX_PATH=$PWD/../deps/prefix
cmake --build build --target THPS64Recomp
cd build && ./THPS64Recomp ../../work/thps.z64
```

`recomp/thps.syms.toml` differs from the THPS64 one: nine functions that the symbol splitter cut short are merged
back to their decomp sizes (the camera FOV setup and parts of the music library; see `thps.syms.toml.orig`).

## Controls (Xbox 360 pad)

Skating is Skate 3's: left stick steers/leans, A pushes, the right stick flicks tricks (down then up = ollie),
triggers grab, Y gets off the board. Start opens THPS's pause menu.

Session markers, as in Skate 3: **LB + d-pad down** sets a session marker, **LB + d-pad up** goes back to it, **LB +
d-pad left/right** switches between the trainer's five marker slots (the selected slot is the session marker). The
Practice tab can also return you there automatically after a bail.

The trainer: **Insert** or **Back + LB + RB**. Tabs: Tuning (pop, grind pop, push speed/power, braking, steering,
wobble, off-board jump, grip, turn power, manual drag, gravity, skater size, hold fakie), Presets (Stock, Mega Pop, Fast,
Moon, Giant, THPS, Realistic, Slow-mo; they stack), Practice (game speed, five marker slots: Home goes to the selected slot,
Shift+Home saves it), Display (Skate 3 camera, Skate 3 animation). Settings are saved in
`~/.config/thpskate3/trainer.ini`.

## Debugging switches

`THPS_SKATE=0` THPS's own skater · `THPS_SKATE_CAM=0` THPS's camera · `THPS_SKATE_POSE=0` THPS's animation · `THPS_SKATE_IK=0` no hand/foot IK · `THPS_SKATE_SOUND=0` no skater sounds ·
`THPS_SKATE_LOG=1` log tricks, events and triggers · `THPS_SKATE_CATCHES=0` no catch log (every bail or sudden stop is
written to `~/.config/thpskate3/catches.log` with a screenshot) · `THPS_SKATE_EDGES=0` skatecore's own edge pairing · `THPS_SKATE_SCALE=units-per-metre` · F9 dumps RDRAM, F10 takes
a screenshot · `tools/autorun.sh` / `tools/into_level.sh` run the game hidden with scripted input (`THPS_PRESS` for
N64 buttons, `THPS_XPAD` for the Xbox pad, `THPS_LEVEL=n` to force a level, `THPS_SKATE_SPAWN=x,y,z,heading` to drop the skater somewhere).
`work/skate_on_thps` (from `tools/native/skate_on_thps.cpp`) runs skatecore headless on a level cache;
`work/wall_probe` (`tools/native/wall_probe.cpp`) rides up every ramp of a level cache and reports where the skater
is stopped, wipes out or never gets air.
