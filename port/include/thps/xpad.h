// Raw Xbox 360 pad snapshots, one per player, in XInput units.
//
// The N64 view of a pad (buttons + one stick) loses the second stick and the
// analogue triggers, and Skate 3's controls need both. The input poll stores
// the full state here before folding it into the N64 one, so the skatecore
// bridge can read exactly what XInputGetState would have returned.
#pragma once
#include <cstdint>

namespace thps::xpad {

// XINPUT_GAMEPAD_* button bits.
enum : uint16_t {
    DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008,
    START = 0x0010, BACK = 0x0020, LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080,
    LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200,
    A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
};

struct State {
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t lx = 0, ly = 0;  // XInput sign: +y is up
    int16_t rx = 0, ry = 0;
};

constexpr int kPlayers = 2;

// Latest snapshot for a player (0 or 1); a disconnected player reads all zero.
State get(int player);
// Written by the input poll (src/callbacks.cpp).
void store(int player, const State& state);

}  // namespace thps::xpad
