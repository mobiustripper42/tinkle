#include "buttons.h"

namespace tinkle {

Buttons::Buttons(IButtonInput& in, const Config& cfg) : in_(in), cfg_(cfg) {}

void Buttons::begin() {
    for (uint8_t i = 0; i < cfg_.count; ++i) {
        in_.configureInput(cfg_.pins[i]);
        // Adopt the boot state as the debounced baseline: a button already held at
        // power-on is not a fresh press, so boot never auto-starts a run (fail dry).
        const bool raw = in_.isDown(cfg_.pins[i]);
        st_[i].lastRaw = raw;
        st_[i].stable  = raw;
    }
}

void Buttons::tick(uint32_t nowMs) {
    for (uint8_t i = 0; i < cfg_.count; ++i) {
        State& s = st_[i];
        s.pressEdge = s.releaseEdge = s.longPressEdge = false;   // edges are per-tick

        const bool raw = in_.isDown(cfg_.pins[i]);
        if (raw != s.lastRaw) {
            // Still bouncing — restart the debounce window, commit nothing.
            s.lastRaw      = raw;
            s.lastChangeMs = nowMs;
        } else if (raw != s.stable &&
                   (uint32_t)(nowMs - s.lastChangeMs) >= cfg_.debounceMs) {
            // Raw held steady through the window and differs from the debounced
            // state — accept the transition. Unsigned subtraction is rollover-safe.
            s.stable = raw;
            if (raw) { s.pressedAtMs = nowMs; s.pressEdge = true; s.longFired = false; }
            else     { s.releaseEdge = true; }
        }

        if (s.stable && !s.longFired &&
            (uint32_t)(nowMs - s.pressedAtMs) >= cfg_.longPressMs) {
            s.longPressEdge = true;
            s.longFired     = true;       // once per hold
        }
    }
}

bool Buttons::pressEdge(uint8_t i)     const { return i < cfg_.count && st_[i].pressEdge; }
bool Buttons::releaseEdge(uint8_t i)   const { return i < cfg_.count && st_[i].releaseEdge; }
bool Buttons::longPressEdge(uint8_t i) const { return i < cfg_.count && st_[i].longPressEdge; }
bool Buttons::isDown(uint8_t i)        const { return i < cfg_.count && st_[i].stable; }

} // namespace tinkle
