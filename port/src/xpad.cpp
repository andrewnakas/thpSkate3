// Raw Xbox 360 pad snapshots (see include/thps/xpad.h).
#include "thps/xpad.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace thps::xpad {
namespace {
std::mutex g_lock;
State g_state[kPlayers];

// THPS_XPAD="A@60-61.5,RY-@70-70.2,RY+@70.2-70.3,LX+@75-78,RT@80-81": player 1 input for unattended runs,
// held between the two times (seconds since the first poll). Buttons: A B X Y LB RB START BACK LS RS DU DD DL DR;
// axes: LX LY RX RY with + / - for full deflection (XInput sign: +Y is up); LT RT for full triggers.
struct Item { std::string what; double from, to; };
std::vector<Item> load_script() {
    std::vector<Item> out;
    const char* e = std::getenv("THPS_XPAD");
    if (e == nullptr) return out;
    std::string s = e;
    size_t p = 0;
    while (p < s.size()) {
        size_t c = s.find(',', p);
        std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        size_t at = it.find('@'), dash = it.find('-', at == std::string::npos ? 0 : at + 1);
        if (at != std::string::npos && dash != std::string::npos)
            out.push_back({it.substr(0, at), std::atof(it.c_str() + at + 1), std::atof(it.c_str() + dash + 1)});
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return out;
}

void apply_script(State& st) {
    static const std::vector<Item> items = load_script();
    if (items.empty()) return;
    static const auto start = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    static const std::pair<const char*, uint16_t> buttons[] = {
        {"A", A}, {"B", B}, {"X", X}, {"Y", Y}, {"LB", LEFT_SHOULDER}, {"RB", RIGHT_SHOULDER}, {"START", START},
        {"BACK", BACK}, {"LS", LEFT_THUMB}, {"RS", RIGHT_THUMB}, {"DU", DPAD_UP}, {"DD", DPAD_DOWN},
        {"DL", DPAD_LEFT}, {"DR", DPAD_RIGHT}};
    for (const Item& it : items) {
        if (t < it.from || t >= it.to) continue;
        st.connected = true;
        for (const auto& [n, m] : buttons) if (it.what == n) st.buttons |= m;
        if (it.what == "LT") st.left_trigger = 255;
        if (it.what == "RT") st.right_trigger = 255;
        if (it.what.size() == 3) {
            const int16_t v = it.what[2] == '+' ? 32767 : -32767;
            const std::string ax = it.what.substr(0, 2);
            if (ax == "LX") st.lx = v;
            if (ax == "LY") st.ly = v;
            if (ax == "RX") st.rx = v;
            if (ax == "RY") st.ry = v;
        }
    }
}
}  // namespace

State get(int player) {
    if (player < 0 || player >= kPlayers) return {};
    State st;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        st = g_state[player];
    }
    if (player == 0) apply_script(st);
    return st;
}

void store(int player, const State& state) {
    if (player < 0 || player >= kPlayers) return;
    std::lock_guard<std::mutex> guard(g_lock);
    g_state[player] = state;
}

}  // namespace thps::xpad
