#pragma once
#include <stdint.h>
#include "run_controller.h"   // RunState — the shared vocabulary
#include "valve_driver.h"     // ValveConfig::MAX_ZONES

// ValveRestMonitor — the DEC-014 auto-return self-test (#52). Verifies a zone
// valve actually RESTS CLOSED after de-energize: the auto-return is
// capacitor-driven and a cap ages over years in heat, so a valve can fail to
// close — invisible to fail-dry (the pump-power gate is the barrier; a stuck-open
// valve passes nothing with the pump off) but an agronomic-correctness and
// maintenance problem: the next run on another zone would silently water this
// one's bed too. This is the detector; it is explicitly NOT a safety layer.
//
// MECHANISM: after a run's CLOSE_ZONE travel completes, the system enters SETTLE
// with the pump long off — residual manifold pressure is all that's left, and a
// healthy close means the flow meter goes quiet. Watch the cumulative pulse count
// from SETTLE entry through IDLE for windowMs: more than maxRestPulses in that
// window means the just-closed zone is still passing water -> flag it. A clean
// full window UN-flags the zone (self-healing — a re-seated valve clears its own
// flag on the next run). A queued run chaining SETTLE -> next aborts the check
// silently (the window would be the next run's flow, not rest flow) — the LAST
// run of a queue session still gets checked, so every used zone is covered.
//
// NON-LATCHING by design: the verdict routes to FaultManager::note() (a log-ring
// entry + the Phase 4 status surface), never to raiseFault — DEC-014 is a
// maintenance heads-up, and halting irrigation over a dribble would be the
// DEC-015 failure mode (a detector blocking watering). A gross leak still
// latches via FlowFaultDetector's idle check (50 pulses / 5 s), which sits a
// decade above this threshold — flag at a dribble, fault at a flood.
//
// Platform-independent (src/core), host-tested with injected pulse counts.

namespace tinkle {

class ValveRestMonitor {
public:
    struct Config {
        // §15 seeds — bench-confirm. The window spans several seconds so a slow
        // dribble registers against maxRestPulses (at K≈1670 p/gal, 0.1 GPM ≈ 2.8
        // pulse/s); TINKLE_SIM shortens it so the check resolves inside a watchable
        // sim session.
#ifdef TINKLE_SIM
        uint32_t windowMs = 3000;
#else
        uint32_t windowMs = 10000;    // REST_WINDOW_MS
#endif
        uint32_t maxRestPulses = 5;   // REST_MAX_PULSES: above this in one window -> flag
    };

    explicit ValveRestMonitor(const Config& cfg) : cfg_(cfg) {}

    // Call every loop tick with the post-tick run state, the zone of the most
    // recently logged run (RunController::lastRun().zoneIndex — pushed on SETTLE
    // entry, so it is current by the time this sees the edge), and the monotonic
    // cumulative pulse count. Returns the zone NEWLY flagged this tick, or -1 —
    // the caller routes a flag to FaultManager::note() and the serial log.
    int tick(RunState state, uint8_t lastRunZone, uint32_t pulses, uint32_t nowMs);

    // Status surface (Phase 4 /api/status; sticky until a clean rest window).
    bool zoneFlagged(uint8_t zone) const {
        return zone < ValveConfig::MAX_ZONES && flagged_[zone];
    }
    uint8_t flaggedMask() const {
        uint8_t m = 0;
        for (uint8_t z = 0; z < ValveConfig::MAX_ZONES; ++z)
            if (flagged_[z]) m |= (uint8_t)(1u << z);
        return m;
    }

private:
    enum class Phase : uint8_t { Idle, Watching };

    Config   cfg_;
    Phase    phase_     = Phase::Idle;
    RunState prevState_ = RunState::Idle;
    uint8_t  zone_      = 0;        // zone under observation
    uint32_t baseline_  = 0;        // pulse count at SETTLE entry
    uint32_t startMs_   = 0;
    bool     flagged_[ValveConfig::MAX_ZONES] = {};
};

} // namespace tinkle
