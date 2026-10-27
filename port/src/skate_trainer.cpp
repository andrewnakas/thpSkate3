// thpSkate3 trainer menu (see include/thps/skate_trainer.h). Modelled on SK8TRAINER: value tabs, stacking presets,
// a practice tab with game speed and five marker slots. Settings persist in <config>/thpskate3_trainer.ini.

#include "thps/skate_trainer.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "imgui.h"
#include "thps/renderer.h"

namespace RT64 {
void SetOverlay(void (*draw)());
};

namespace thps::trainer {
namespace {

std::mutex g_lock;
Settings g_settings;
Status g_status;
std::array<bool, kMarkers> g_saved{};
int g_request_slot = -1;
bool g_request_save = false;
std::atomic<int> g_selected_slot{0};

std::filesystem::path settings_path() {
    const char* home = std::getenv("HOME");
    std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "thpskate3" : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "trainer.ini";
}

struct Field { const char* key; float Settings::*f; float lo, hi; const char* label; const char* help; };
const Field kFields[] = {
    {"pop", &Settings::pop, 0.25f, 4.0f, "Pop", "Ollie and flip-trick jump height"},
    {"grind_pop", &Settings::grind_pop, 0.25f, 4.0f, "Grind pop", "Jump height out of grinds"},
    {"push_speed", &Settings::push_speed, 0.25f, 4.0f, "Push top speed", "Fastest speed pushing reaches"},
    {"push_power", &Settings::push_power, 0.25f, 4.0f, "Push power", "Speed each push adds"},
    {"braking", &Settings::braking, 0.0f, 4.0f, "Braking", "Powerslide / foot-brake strength"},
    {"steering", &Settings::steering, 0.25f, 3.0f, "Steering", "How hard the board carves"},
    {"wobble", &Settings::wobble, 0.0f, 3.0f, "Speed wobble", "Wobble at high speed"},
    {"offboard_jump", &Settings::offboard_jump, 0.25f, 4.0f, "Off-board jump", "Jump height on foot"},
    {"grip", &Settings::grip, 0.25f, 3.0f, "Grip", "Wheel and slide friction"},
    {"turn_power", &Settings::turn_power, 0.25f, 3.0f, "Turn power", "Heading change strength"},
    {"manual_drag", &Settings::manual_drag, 0.0f, 3.0f, "Manual drag", "Slow-down while manualling"},
    {"gravity", &Settings::gravity, 0.1f, 2.0f, "Gravity", "World gravity (0.4 = moon)"},
    {"game_speed", &Settings::game_speed, 0.05f, 2.0f, "Game speed", "Simulation speed: slow motion below 1"},
    {"skater_size", &Settings::skater_size, 0.75f, 2.0f, "Skater size",
     "Skater, board, pops and speed against the THPS world (THPS levels are bigger than Skate 3's)"},
};

void save_locked() {
    std::ofstream f(settings_path());
    for (const Field& fd : kFields) f << fd.key << '=' << g_settings.*(fd.f) << '\n';
    f << "hold_fakie=" << int(g_settings.hold_fakie) << '\n';
    f << "skate3_camera=" << int(g_settings.skate3_camera) << '\n';
    f << "skate3_pose=" << int(g_settings.skate3_pose) << '\n';
    f << "auto_return=" << int(g_settings.auto_return) << '\n';
}

void load() {
    std::ifstream f(settings_path());
    std::string line;
    std::lock_guard<std::mutex> g(g_lock);
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const float v = std::strtof(line.c_str() + eq + 1, nullptr);
        for (const Field& fd : kFields)
            if (k == fd.key) g_settings.*(fd.f) = v < fd.lo ? fd.lo : (v > fd.hi ? fd.hi : v);
        if (k == "hold_fakie") g_settings.hold_fakie = v != 0;
        if (k == "skate3_camera") g_settings.skate3_camera = v != 0;
        if (k == "skate3_pose") g_settings.skate3_pose = v != 0;
        if (k == "auto_return") g_settings.auto_return = v != 0;
    }
}

struct Preset { const char* name; const char* help; void (*apply)(Settings&); };
const Preset kPresets[] = {
    {"Stock", "Everything back to Skate 3's own values", [](Settings& s) { s = Settings{}; }},
    {"Mega Pop", "Every pop x3", [](Settings& s) { s.pop = 3.0f; s.grind_pop = 3.0f; }},
    {"Fast", "Push top speed and power x3", [](Settings& s) { s.push_speed = 3.0f; s.push_power = 3.0f; }},
    {"Moon", "Gravity x0.4", [](Settings& s) { s.gravity = 0.4f; }},
    {"Giant", "Skater size x1.6: THPS-sized lines and gaps", [](Settings& s) { s.skater_size = 1.6f; }},
    {"THPS", "Arcade feel: higher pops, faster pushing, lighter gravity, no wobble",
     [](Settings& s) { s.pop = 1.8f; s.grind_pop = 1.8f; s.push_speed = 1.8f; s.push_power = 2.0f; s.gravity = 0.8f; s.wobble = 0.0f; s.steering = 1.3f; }},
    {"Realistic", "Smaller pops, weaker pushes, more grip",
     [](Settings& s) { s.pop = 0.8f; s.grind_pop = 0.8f; s.push_power = 0.8f; s.grip = 1.3f; }},
    {"Slow-mo", "Half-speed simulation", [](Settings& s) { s.game_speed = 0.5f; }},
};

void draw() {
    if (!is_open()) return;
    static bool first = true;
    if (first) {
        first = false;
        std::fprintf(stderr, "[skate] trainer window drawn\n");
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    Settings s;
    Status st;
    std::array<bool, kMarkers> saved;
    {
        std::lock_guard<std::mutex> g(g_lock);
        s = g_settings;
        st = g_status;
        saved = g_saved;
    }
    const Settings before = s;
    if (ImGui::Begin("thpSkate3 trainer  (Insert / Back+LB+RB)")) {
        ImGui::Text("%s   %.0f km/h   %s", st.level.c_str(), st.speed_kmh, st.state.c_str());
        if (!st.trick.empty()) ImGui::Text("Last trick: %s", st.trick.c_str());
        ImGui::Separator();
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Tuning")) {
                for (const Field& fd : kFields) {
                    if (fd.f == &Settings::game_speed) continue;
                    ImGui::SliderFloat(fd.label, &(s.*(fd.f)), fd.lo, fd.hi, "x%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", fd.help);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) s.*(fd.f) = Settings{}.*(fd.f);
                }
                ImGui::Checkbox("Hold fakie", &s.hold_fakie);
                ImGui::TextDisabled("Middle-click a slider to reset it.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Presets")) {
                for (const Preset& p : kPresets) {
                    if (ImGui::Button(p.name, ImVec2(140, 0))) p.apply(s);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(p.help);
                }
                ImGui::TextDisabled("Presets stack (except Stock).");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Practice")) {
                ImGui::SliderFloat("Game speed", &s.game_speed, 0.05f, 2.0f, "x%.2f");
                ImGui::Separator();
                for (int i = 0; i < kMarkers; i++) {
                    ImGui::PushID(i);
                    if (ImGui::RadioButton("", g_selected_slot == i)) g_selected_slot = i;
                    ImGui::SameLine();
                    ImGui::Text("Slot %d %s", i + 1, saved[size_t(i)] ? "(saved)" : "(empty)");
                    ImGui::SameLine();
                    if (ImGui::Button("Save here")) request_marker(i, true);
                    ImGui::SameLine();
                    if (ImGui::Button("Go") && saved[size_t(i)]) request_marker(i, false);
                    ImGui::PopID();
                }
                ImGui::Checkbox("Auto-return after a bail", &s.auto_return);
                ImGui::TextDisabled("The selected slot is your session marker.");
                ImGui::TextDisabled("Pad: LB + d-pad down = set session, LB + d-pad up = go to session,");
                ImGui::TextDisabled("     LB + d-pad left/right = pick the slot.");
                ImGui::TextDisabled("Keyboard: Home = go to the selected slot, Shift+Home = save it.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                ImGui::Checkbox("Skate 3 camera", &s.skate3_camera);
                ImGui::Checkbox("Skate 3 animation on the THPS character", &s.skate3_pose);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!(s == before)) set(s);
}

}  // namespace

Settings get() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_settings;
}

void set(const Settings& s) {
    std::lock_guard<std::mutex> g(g_lock);
    g_settings = s;
    save_locked();
}

void request_marker(int slot, bool save) {
    std::lock_guard<std::mutex> g(g_lock);
    g_request_slot = slot;
    g_request_save = save;
}

bool take_marker_request(int& slot, bool& save) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_request_slot < 0) return false;
    slot = g_request_slot;
    save = g_request_save;
    g_request_slot = -1;
    return true;
}

void set_marker_saved(int slot, bool saved_) {
    std::lock_guard<std::mutex> g(g_lock);
    if (slot >= 0 && slot < kMarkers) g_saved[size_t(slot)] = saved_;
}

void set_status(const Status& s) {
    std::lock_guard<std::mutex> g(g_lock);
    g_status = s;
}

void install() {
    load();
    RT64::SetOverlay(&draw);
}

// Requests only: the overlay is created and destroyed on RT64's main thread (service(), from the port's main loop),
// as RT64 does for its own inspector; the pad combo is read on a game thread.
std::atomic<bool> g_toggle_requested{false};
void toggle() { g_toggle_requested = true; }
void service() {
    if (g_toggle_requested.exchange(false)) thps::set_overlay_open(!thps::overlay_open());
}
bool is_open() { return thps::overlay_open(); }

int selected_slot() { return g_selected_slot; }
void select_slot(int slot) { g_selected_slot = (slot % kMarkers + kMarkers) % kMarkers; }

}  // namespace thps::trainer
