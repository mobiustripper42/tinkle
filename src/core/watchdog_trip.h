#pragma once
#include <stdint.h>

// WatchdogTrip — the ATtiny85's trip logic (firmware spec §9), header-only and
// platform-independent so the SAME unit compiles into the attiny85 binary and the
// native test runner (DEC-016). The sketch owns the pins; this owns the decision.
// No Arduino API, no interrupts — poll the heartbeat level into tick() with the
// ATtiny's OWN millis(). It never trusts the ESP32's sense of time.
//
// ENCODING (the §9 "pick one and document it" decision):
//   The ESP32 emits the heartbeat square wave ONLY while the pump is commanded
//   (START_PUMP/RUNNING) — so "heartbeat present" == "water can move". Between
//   queued runs the close-travel + settle + next prep/open gap (>= ~21 s real,
//   >= ~3 s sim) far exceeds HB_TIMEOUT_MS, so the trip logic disarms and re-arms
//   per run and HARD_MAX_RUNTIME is a true per-run ceiling.
//
// States:
//   DISARMED  ARM low. The rest state — power-up/reset default (fail dry on
//             watchdog reboot) and wherever quiet leads.
//   ARMED     ARM high. Entered on a heartbeat edge; held while edges keep
//             arriving within HB_TIMEOUT_MS and armed time < HARD_MAX_RUNTIME.
//   LOCKOUT   ARM low, TRIPPED asserted. Entered only on the HARD_MAX_RUNTIME
//             ceiling — the unambiguous fault (the ESP32 is alive and still
//             claiming run-active long past its own software stop). Held while
//             the heartbeat keeps coming; releases to DISARMED after the line
//             goes quiet for HB_TIMEOUT_MS (the ESP32 latched FAULT_WATCHDOG,
//             safed, and stopped beating — that quiet IS the resolved signal
//             the §14 clear gate keys on).
//
// Heartbeat quiet while ARMED disarms SILENTLY (no TRIPPED): a clean run end is
// indistinguishable from a stalled ESP32 on this encoding, and a stalled ESP32
// cannot read the trip line anyway — opening the relay is the entire response.
// TRIPPED is reserved for the one condition the ESP32 can act on.

namespace tinkle {

class WatchdogTrip {
public:
    struct Config {
        uint32_t hbTimeoutMs = 2000;                   // §15 HB_TIMEOUT_MS
        uint32_t hardMaxMs   = 30ul * 60ul * 1000ul;   // §15 HARD_MAX_RUNTIME
    };

    enum class State : uint8_t { Disarmed, Armed, Lockout };

    explicit WatchdogTrip(const Config& cfg) : cfg_(cfg) {}

    // Capture the initial line level WITHOUT treating it as an edge, so a level
    // frozen mid-stream at power-up can never arm. Power-up default: DISARMED.
    void begin(bool hbLevel, uint32_t nowMs) {
        lastLevel_  = hbLevel;
        lastEdgeMs_ = nowMs;
        state_      = State::Disarmed;
    }

    // Poll every loop pass with the current heartbeat line level. All intervals
    // are uint32_t subtractions, so the ~49.7-day millis wrap is harmless.
    void tick(bool hbLevel, uint32_t nowMs) {
        const bool edge = (hbLevel != lastLevel_);
        lastLevel_ = hbLevel;
        if (edge) lastEdgeMs_ = nowMs;

        switch (state_) {
            case State::Disarmed:
                if (edge) { state_ = State::Armed; armStartMs_ = nowMs; }
                break;
            case State::Armed:
                // Ceiling first: a beating-but-overrun ESP32 must not stay armed.
                if ((uint32_t)(nowMs - armStartMs_) >= cfg_.hardMaxMs)
                    state_ = State::Lockout;
                else if ((uint32_t)(nowMs - lastEdgeMs_) >= cfg_.hbTimeoutMs)
                    state_ = State::Disarmed;          // clean run end — silent
                break;
            case State::Lockout:
                if ((uint32_t)(nowMs - lastEdgeMs_) >= cfg_.hbTimeoutMs)
                    state_ = State::Disarmed;          // ESP32 safed + went quiet
                break;
        }
    }

    bool  armed()   const { return state_ == State::Armed; }
    bool  tripped() const { return state_ == State::Lockout; }
    State state()   const { return state_; }

private:
    Config   cfg_;
    State    state_      = State::Disarmed;
    bool     lastLevel_  = false;
    uint32_t lastEdgeMs_ = 0;
    uint32_t armStartMs_ = 0;
};

} // namespace tinkle
