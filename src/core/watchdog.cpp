#include "watchdog.h"

namespace tinkle {

void Watchdog::begin(uint32_t nowMs) {
    g_.configureOutput(pin_);
    level_    = false;
    emitting_ = false;
    g_.write(pin_, false);
    lastToggleMs_ = nowMs;
}

Fault Watchdog::tick(RunState state, bool tripAsserted, uint32_t nowMs) {
    // Heartbeat window = pump commanded. STOP_PUMP is already outside it: the pump
    // FET drops on StopPump ENTRY, so the beat may stop with it — the ATtiny stays
    // armed through its 2 s timeout anyway, covering the de-energize tail.
    const bool window = (state == RunState::StartPump || state == RunState::Running);

    if (window && !emitting_) {
        emitting_ = true;                 // first edge immediately — arms the ATtiny
        level_ = !level_;
        g_.write(pin_, level_);
        lastToggleMs_ = nowMs;
    } else if (window) {
        if ((uint32_t)(nowMs - lastToggleMs_) >= cfg_.heartbeatMs) {
            level_ = !level_;
            g_.write(pin_, level_);
            lastToggleMs_ = nowMs;
        }
    } else if (emitting_) {
        emitting_ = false;                // park LOW between runs (quiet = disarm)
        level_ = false;
        g_.write(pin_, false);
    }

    // Trip qualification (DEC-023): a raw assertion must hold continuously for
    // tripConfirmMs before it counts. Any released read restarts the clock.
    if (tripAsserted) {
        if (!tripSeen_) { tripSeen_ = true; tripStartMs_ = nowMs; }
        tripConfirmed_ = (uint32_t)(nowMs - tripStartMs_) >= cfg_.tripConfirmMs;
    } else {
        tripSeen_      = false;
        tripConfirmed_ = false;
    }

    const bool active = (state != RunState::Idle && state != RunState::Fault);
    return (tripConfirmed_ && active) ? Fault::Watchdog : Fault::None;
}

} // namespace tinkle
