// thpSkate3 trainer: an sk8trainer-style tuning and practice menu for the skatecore skater, drawn in RT64's ImGui
// overlay (src/skate_trainer.cpp). Settings are shared between the menu (render thread) and the bridge (game
// thread, src/skate_bridge.cpp), which applies them every frame.
#pragma once
#include <string>

namespace thps::trainer {

struct Settings {
    // skatecore TrainerTuning (skate-mods schema): multipliers on the stock values. The defaults are the tuning
    // thpSkate3 is played with (higher pop and push, more grip and off-board jump, a 1.5x skater); the Stock preset
    // gives Skate 3's own values.
    float pop = 1.5f;
    float grind_pop = 1.48f;
    float push_speed = 2.09f;
    float push_power = 2.07f;
    float braking = 1.0f;
    float steering = 1.0f;
    float wobble = 1.0f;
    float offboard_jump = 2.11f;
    float grip = 1.22f;
    float turn_power = 1.0f;
    float manual_drag = 1.0f;
    bool hold_fakie = false;
    float gravity = 1.0f;       // world gravity multiplier
    float game_speed = 1.0f;    // simulation speed (0.05 .. 2): slow motion below 1
    // Skater size against THPS's world (THPS levels are built bigger than Skate 3's): the Skate 3 skater, board,
    // pops and speeds and the THPS character all scale by it.
    float skater_size = 1.5f;
    bool auto_return = false;   // after a bail, go back to the session marker (the selected slot)
    bool skate3_camera = true;  // Skate 3's camera (else THPS's)
    bool skate3_pose = true;    // Skate 3 animation on the THPS character (else THPS's own animation)
    bool operator==(const Settings&) const = default;
};

Settings get();
void set(const Settings& s);

// Practice markers: the menu asks, the bridge (game thread) does it. The selected slot is the session marker the
// pad sets and returns to, as in Skate 3: LB + d-pad down sets it, LB + d-pad up goes there, LB + d-pad left/right
// picks the slot.
constexpr int kMarkers = 5;
void request_marker(int slot, bool save);
int selected_slot();
void select_slot(int slot);
bool take_marker_request(int& slot, bool& save);
void set_marker_saved(int slot, bool saved);

// What the menu shows about the skater (written by the bridge).
struct Status {
    float speed_kmh = 0.0f;
    std::string state;
    std::string trick;
    std::string level;
};
void set_status(const Status& s);

// Menu open/close (Insert, or Back + LB + RB on the pad). While it is open the pad drives the menu.
void install();   // registers the RT64 overlay callback and loads saved settings
void toggle();    // request; applied by service() on the main thread
void service();   // called every frame from the port's main loop
bool is_open();

}  // namespace thps::trainer
