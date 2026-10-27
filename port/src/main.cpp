// THPS64 recomp -- entry point.
//
// Adapted from Pilotwings 64: Recompiled (MIT, see LICENSE.pilotwings64-recomp): registers the game with
// librecomp, supplies the platform callbacks ultramodern needs, and starts the runtime. The recompiled game
// code is reached through recomp_entrypoint (N64Recomp, recomp/thps.toml).
//
// This program contains no game data: the player supplies their own Tony Hawk's Pro Skater (USA, V1.1) dump.

#include "thps/callbacks.h"
#include "thps/crash_handler.h"
#include "thps/renderer.h"
#include "thps/rom.h"
#include "thps/glue.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#   include <crtdbg.h>
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <librecomp/game.hpp>
#include <ultramodern/ultramodern.hpp>

#ifndef THPS_VERSION
#   define THPS_VERSION "0.0.0"
#   define THPS_VERSION_MAJOR 0
#   define THPS_VERSION_MINOR 0
#   define THPS_VERSION_PATCH 0
#endif

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

namespace thps {

std::filesystem::path settings_directory() {
    if (std::filesystem::exists("portable.txt")) {
        return std::filesystem::current_path();
    }
    const char* base = std::getenv(
#if defined(_WIN32)
        "LOCALAPPDATA"
#else
        "HOME"
#endif
    );
    return base ? std::filesystem::path{ base } / "THPS64Recomp" : std::filesystem::current_path();
}

}  // namespace thps

namespace {

void on_init(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::printf("[thps] runtime initialised; rdram base = %p\n", static_cast<void*>(rdram));
    thps::set_rdram_base(rdram);
    // main (vram 0x80016AC0) is not where librecomp's 1 MB boot load puts it: register it at its real address.
    thps::place_resident_sections(thps::kEntrypoint);
}

void on_thread_create(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    static int count = 0;
    std::printf("[thps] game thread %d created: OSThread 0x%08X id %d entry 0x%08X\n", ++count,
                static_cast<uint32_t>(ctx->r4), static_cast<int32_t>(ctx->r5), static_cast<uint32_t>(ctx->r6));
}

int identify(const char* path) {
    thps::RomHeader header;
    std::string error;
    if (!thps::read_header(path, header, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%-12s %s\n%-12s %s\n%-12s 0x%08X 0x%08X\n", "file", path, "name", header.internal_name.c_str(),
                "header crc", header.crc1, header.crc2);
    std::vector<std::string> problems;
    if (thps::verify(header, problems)) {
        std::printf("\nThis dump matches the pinned target.\n");
        return 0;
    }
    std::printf("\nThis dump does NOT match the pinned target:\n");
    for (const std::string& p : problems) {
        std::printf("  - %s\n", p.c_str());
    }
    return 2;
}

int run(int argc, char** argv, const char* rom_path) {
    {
        thps::RomHeader header;
        std::string error;
        std::vector<std::string> problems;
        if (!thps::read_header(rom_path, header, error) || !thps::verify(header, problems)) {
            std::fprintf(stderr, "error: %s\n", error.empty() ? "dump does not match the pinned target (try --identify)"
                                                              : error.c_str());
            return 2;
        }
    }
    {
        const std::filesystem::path settings = thps::settings_directory();
        std::error_code ec;
        std::filesystem::create_directories(settings, ec);
        recomp::register_config_path(settings);
    }

    thps::register_sections();
    thps::register_patches();

    recomp::GameEntry game{};
    game.rom_hash = thps::kRomHash;
    game.internal_name = "TONY HAWK PRO SKATER";
    game.display_name = thps::kDisplayName;
    game.game_id = thps::kGameId;
    game.mod_game_id = thps::kModGameId;
    game.save_type = recomp::SaveType::None;   // THPS saves to the Controller Pak (osPfs*), not the cartridge
    game.is_enabled = true;
    game.entrypoint_address = static_cast<gpr>(static_cast<int32_t>(thps::kEntrypoint));
    game.entrypoint = recomp_entrypoint;
    game.on_init_callback = on_init;
    game.thread_create_callback = on_thread_create;
    if (!recomp::register_game(game)) {
        std::fprintf(stderr, "error: failed to register the game with librecomp\n");
        return 1;
    }
    const recomp::RomValidationError rom_error =
        recomp::select_rom(std::filesystem::path{ rom_path }, std::u8string{ thps::kGameId });
    if (rom_error != recomp::RomValidationError::Good) {
        std::fprintf(stderr, "error: librecomp rejected the dump (%d)\n", static_cast<int>(rom_error));
        return 2;
    }

    recomp::Configuration config{};
    config.argc = argc;
    config.argv = argv;
    config.project_version = recomp::Version{ THPS_VERSION_MAJOR, THPS_VERSION_MINOR, THPS_VERSION_PATCH, "" };
    config.rsp_callbacks = thps::rsp_callbacks();
    config.renderer_callbacks = thps::renderer_callbacks();
    config.audio_callbacks = thps::audio_callbacks();
    config.input_callbacks = thps::input_callbacks();
    config.gfx_callbacks = thps::gfx_callbacks();
    config.events_callbacks = thps::events_callbacks();
    config.error_handling_callbacks = thps::error_handling_callbacks();
    config.threads_callbacks = thps::threads_callbacks();

    // start_game must come before start(): start() spawns the game thread and then runs the main loop.
    recomp::start_game(std::u8string{ thps::kGameId }, std::string{});
    recomp::start(config);
    thps::shutdown_platform();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    thps::install_crash_handler();
#if defined(_WIN32) && !defined(NDEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("Usage:\n  %s <rom.z64>             run the game\n  %s --identify <rom.z64>  check a dump\n\n"
                    "This program contains no game data. Supply your own Tony Hawk's Pro Skater (USA, V1.1) dump.\n",
                    argv[0], argv[0]);
        return 1;
    }
    const std::string command = argv[1];
    if (command == "--identify") {
        return argc < 3 ? 1 : identify(argv[2]);
    }
    return run(argc, argv, argv[1]);
}
