#pragma once
// Hooks the Pilotwings-derived glue calls that THPS has no use for yet (scripted input, game-state watcher).
#include <cstdint>

namespace thps {
void input_script_state(uint16_t* buttons, float* stick_x, float* stick_y);
void input_script_state(int player, uint16_t* buttons, float* stick_x, float* stick_y);
void set_rdram_base(uint8_t* rdram);
uint8_t* rdram_base();
void poll_game_state();
}  // namespace thps
