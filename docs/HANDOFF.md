# Handoff: Windows and macOS builds

## Update 2026-10-29: Windows is done (v0.1.2)

- **Released:** v0.1.2 (prerelease), `thpSkate3-0.1.2-windows-x86_64.zip` (17 MB, `tools/make_release_win.sh 0.1.2`).
  Built on Windows 11 with clang-cl 23 + VS 2022 Build Tools + Ninja; RT64 runs on D3D12.
- **Checked from a fresh unpack of the zip:** `setup.bat ROM ISO` (Git Bash + uv) built the level caches and
  core.pack; the exe, started from another directory, booted into Warehouse (`SkWare ... spawned`,
  `SKATE_GRIND_READY splines=41`), pushed down the start ramp and scored an OLLIE from a scripted pad
  (`THPS_XPAD`), and quit cleanly. Screenshots in the scripted runs show the level, skater and HUD.
- **What changed for Windows:**
  - Root found with `SDL_GetBasePath()` (no `/proc/self/exe`); paths printed with `.string()`.
  - `thps::trainer::config_dir()`: `%APPDATA%	hpskate3`, `~/Library/Application Support/thpskate3` on macOS,
    `~/.config/thpskate3` elsewhere.
  - `import_skate3.sh` / `setup.sh`: venv `Scripts/python.exe` on Windows; `python` when `python3` is missing.
  - skatecore: clang-cl silently drops `-ffp-contract=off` (it ends up `on`), so MSVC builds pass
    `/clang:-ffp-contract=off`.
  - `patches/plume.patch`: D3D12 `copyTextureRegion` no longer reads a null destination texture on texture ->
    buffer copies (the frame capture crashed the present thread).
  - `thpskate3.bat`, `setup.bat` (finds Git for Windows' bash), `.gitattributes` (LF for `.sh`/`.patch`), and the
    zip bundles the MSVC C++ runtime DLLs app-local.
- **Traps:** the N64Recomp target is `N64RecompCLI`; `recomp_loop.py` needs an absolute N64Recomp path (it runs
  from `recomp/`); global `core.autocrlf=true` breaks the patches (setup_deps.sh was run with it overridden to
  false); a scripted run's screenshots need the plume fix.
- **Not done:** macOS. Linux was not rebuilt for 0.1.2 (the Linux changes are the root lookup and config dir);
  Linux users stay on 0.1.1 until it is.


Written 2026-10-28 on Linux, at commit `9e2a29a` on `main`.

## Where things stand

- **Released:** [v0.1.1](https://github.com/andrewnakas/thpSkate3/releases/tag/v0.1.1) (prerelease), Linux x86-64
  only, `thpSkate3-0.1.1-linux-x86_64.tar.gz` (6.6 MB, made by `tools/make_release.sh 0.1.1`).
- **Checked:** the packaged setup was run end to end from a fresh unpack of that tarball. With a ROM and an ISO
  in, `tools/setup.sh` took 2 minutes, `./thpskate3` booted into Warehouse with the skater, the HUD and 41 grind
  rails, and there were no errors.
- **Rules that carry over to every platform:**
  - Ship nothing from either game. A release holds only the built game, its runtime libraries, the setup tools
    and the docs.
  - Users import from their own disc: the THPS N64 US V1.1 ROM and the Xbox 360 Skate 3 ISO.
  - The game binary contains recompiled THPS code, which is fine, since it does nothing without the ROM.
  - License is GPL-3.0 (because of skatecore-cpp). Ship `LICENSE`, `LICENSE.thps64` and
    `LICENSE.pilotwings64-recomp` with every build.
- **Commit identity:** `andrewnakas <7544678+andrewnakas@users.noreply.github.com>`.

## What a Windows or Mac build needs

The port is built on the Pilotwings 64 recomp base (N64Recomp, N64ModernRuntime and RT64), which already
supports Windows (D3D12) and macOS (Metal). `port/CMakeLists.txt` has `WIN32` and `APPLE` branches inherited
from it, but neither has been built for thpSkate3 yet. Expect the first build on each OS to need fixes.

### 1. Tools

- **Windows:**
  - Git for Windows (its Git Bash runs the `.sh` scripts), Python 3, [uv](https://docs.astral.sh/uv/), CMake,
    Ninja, and LLVM clang (`clang-cl`, or Visual Studio 2022 with the Clang tools).
  - RT64 brings its own SDL2 on Windows: `lib/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3`.
- **macOS:**
  - Xcode command-line tools, Homebrew `cmake ninja python sdl2 uv`.
  - On Apple Silicon, check that the arm64 branch in `port/CMakeLists.txt` (around line 236) builds.

### 2. Source build (README, "Setup (Linux, from source)")

Same steps on every OS:

1. Clone this repo and the [THPS64 decomp](https://github.com/andrewnakas/thps64). Set `THPS64=` to the decomp
   checkout. It provides `tools/build_main.py` and `tools/erz.py`.
2. Run `tools/setup_deps.sh` (Git Bash on Windows). It clones skatecore-cpp and pilotwings-64-recomp at pinned
   revisions and applies `patches/*.patch`.
3. Unpack the ROM and recompile it:
   - `build_main.py`
   - `make_recomp_rom.py`
   - build N64Recomp and RSPRecomp
   - `recomp_loop.py`
   - RSPRecomp on `aspMain.thps.toml`
   - `gen_reimplemented_decls.py`

   `recomp/RecompiledFuncs/` (18 MB of generated C) is not in git, so it has to be generated on each machine from
   the ROM.
4. Configure `port/` with `-DTHPS_WITH_RUNTIME=ON -DTHPS_WITH_RECOMPILED=ON -DTHPS_LIB_DIR=<deps/pw64/lib>` and
   build the `THPS64Recomp` target.
5. Run `tools/setup.sh ROM ISO` for the game data, then start the game.

### 3. Linux-only code to fix (known)

| Where | What | Fix |
|---|---|---|
| `port/src/skate_bridge.cpp` ~line 207 | Finds the repo root from `/proc/self/exe`. Without it, it falls back to the current directory, so the game only finds `data/` and `work/` if started from the repo root. | Use `SDL_GetBasePath()`. The exe sits in `<root>/port/build/`. |
| `port/src/skate_trainer.cpp` ~line 34, `skate_bridge.cpp` ~line 945 | Settings (`trainer.ini`) and the catch log go to `$HOME/.config/thpskate3`. `HOME` is usually unset on Windows, so they land in the current directory. | Windows: `%APPDATA%\thpskate3`. macOS: `~/Library/Application Support/thpskate3`, or keep `~/.config`. |
| `tools/import_skate3.sh` | The converter's venv Python is `$VENV/bin/python`. On Windows it is `$VENV/Scripts/python.exe`. | Pick the right path by OS. |
| `tools/*.sh`, `tools/*.py` | Call `python3`, which is often only `python` on Windows. | Detect it, or run under Git Bash with a `python3` alias. |
| `thpskate3` launcher | `sh` with `LD_LIBRARY_PATH`. | Windows: a `thpskate3.bat`, with DLLs next to the exe. macOS: `DYLD_LIBRARY_PATH`, or bundle the dylibs with an rpath. |
| `tools/make_release.sh` | Linux packaging: `strip`, `libSDL2-2.0.so.0`, `.tar.gz`. | Windows: zip with `THPS64Recomp.exe`, `SDL2.dll`, the dxc/dxil DLLs RT64 needs at runtime, a `.bat` launcher, `tools/` and docs. macOS: `.tar.gz` or `.zip`, with SDL2 dylib and codesign/quarantine notes. |
| `tools/autorun.sh`, `tools/into_level.sh` | Scripted hidden test runs. They use env vars (`THPS_HIDDEN`, `THPS_PRESS`, `THPS_LEVEL`, ...), so they should work under Git Bash with `.exe`. | Optional. |

Unix-only calls in the port beyond these haven't been audited. Grep for `/proc`, `getenv("HOME")`,
`unistd`, `dlfcn` and `pthread` once it compiles.

### 4. Check a build

- Boot and drive into Warehouse:
  `THPS_LEVEL=0 tools/into_level.sh check 80 75` (or the env vars in it by hand). In `stderr.log`, look for
  `[skate] SkWare: ... spawned` and `SKATE_GRIND_READY splines=41`.
- Headless physics on the level caches (no game window needed, so it's a quick sanity check of the skatecore
  build):
  - `work/fall_probe data/cleanpack/core.pack work/levels/SkSchl.thl 60 150`. The out-of-level count is
    usually a few in 60 runs.
  - `work/wall_probe ...` likewise.

  Build both with clang++ (see the README's debugging section). Same numbers as on Linux means the physics
  build is right (skatecore needs strict floats: `-ffp-contract=off`, no fast-math).
- Then play, so you notice anything the scripted checks don't catch: pad input, sound, the trainer overlay
  (Insert / Back+LB+RB), and the RT64 renderer.

### 5. Release

- Upload the Windows and Mac archives to v0.1.1. Use the same version, since nothing in the game changed; cut a
  new version if code had to change.
  `gh release upload v0.1.1 <archive>`, then update the notes: "What you need", "Run it" per OS, and remove
  "Linux only for now".

## What changed in 0.1.1 (for context)

- **Level collision** (`tools/thps_level.py`):
  - Triangles are decoded with the vertices loaded at the time. Batches with two vertex loads used to give holes
    and face-down floors.
  - Faces in batches the game never draws (flag `0x800`) aren't solid when they're walls, steep faces or
    ceilings.
  - Undrawn floors stay solid, and so do the rail fins: undrawn faces within 12 units of a THPS rail and not
    above it.
  - Every quarter-pipe coping gets a grind rail (`coping_rails`).
- **THPS vert:** skatecore's launch gained `thps_vert_max_ground_normal_y`. thpSkate3 sets it to 0.66, so off
  ramps of 49° or steeper the speed toward the coping goes straight up (`Sim::set_thps_vert`, in
  `patches/skatecore-cpp.patch`).
- **Out-of-level rescue:** if the skater has nothing under it for 20 frames, it's put back where it last rolled.
- **Probes:** `tools/native/fall_probe.cpp` (falls and out-of-world) and `wall_probe.cpp` (ramp stops, air
  hits, fly-outs; `VERTY`, `RAMPMAXNY`).

## Open issues

- Steep ramps keep THPS's low-poly kinks (for example, School's pool goes from 18° to 54° in one step), which
  can slow the skater. Rounding them used to move surfaces off what's drawn, so it needs an approach that stays
  on the drawn shape.
- Warehouse's start ramp (14 m) needs about 49 km/h at the bottom; pushing tops out around 27 km/h. Options: a
  THPS-speed setting, or a ramp assist that holds speed while climbing.
- Warehouse's half-pipe lip (85°) and some coping stalls can still bail.
