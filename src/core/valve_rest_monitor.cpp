#include "valve_rest_monitor.h"

namespace tinkle {

int ValveRestMonitor::tick(RunState state, uint8_t lastRunZone, uint32_t pulses,
                           uint32_t nowMs) {
    int newlyFlagged = -1;

    if (state == RunState::Settle && prevState_ != RunState::Settle) {
        // A zone just finished its close travel. Real hydraulics are still trailing
        // (draindown, a valve seating under residual pressure — #124), so don't
        // baseline the rest window yet: wait for the flow to quiesce first.
        phase_ = Phase::Draining;
        zone_  = lastRunZone;
        drain_.open(pulses, nowMs);
    } else if (phase_ != Phase::Idle) {
        const bool resting = (state == RunState::Settle || state == RunState::Idle);
        if (!resting) {
            // Chained run or fault took over the line — the window would measure
            // the next run's flow, not rest flow. Abort, no verdict either way.
            phase_ = Phase::Idle;
        } else if (phase_ == Phase::Draining) {
            if (drain_.drained(pulses, nowMs)) {
                if (drain_.capped()) {
                    // Flow never quieted inside the cap: water is still passing long
                    // past any draindown — the DEC-014 failure itself. Flag directly.
                    if (zone_ < ValveConfig::MAX_ZONES && !flagged_[zone_]) {
                        flagged_[zone_] = true;
                        newlyFlagged = (int)zone_;
                    }
                    phase_ = Phase::Idle;
                } else {
                    // Drained clean — open the rest window on a quiet meter.
                    phase_    = Phase::Watching;
                    baseline_ = pulses;
                    startMs_  = nowMs;
                }
            }
        } else if ((uint32_t)(pulses - baseline_) > cfg_.maxRestPulses) {
            if (zone_ < ValveConfig::MAX_ZONES && !flagged_[zone_]) {
                flagged_[zone_] = true;
                newlyFlagged = (int)zone_;
            }
            phase_ = Phase::Idle;
        } else if ((uint32_t)(nowMs - startMs_) >= cfg_.windowMs) {
            // Full window, quiet meter: the valve rests closed. Self-heal.
            if (zone_ < ValveConfig::MAX_ZONES) flagged_[zone_] = false;
            phase_ = Phase::Idle;
        }
    }

    prevState_ = state;
    return newlyFlagged;
}

} // namespace tinkle
