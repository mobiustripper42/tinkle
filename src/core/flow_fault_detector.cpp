#include "flow_fault_detector.h"

namespace tinkle {

Fault FlowFaultDetector::update(RunState state, float rateGPM, uint32_t pulses, uint32_t nowMs) {
    // --- edge bookkeeping (uses prevState_, so do it before the checks) ---
    if (state == RunState::Running && prevState_ != RunState::Running)
        runStartMs_ = nowMs;                         // restart the grace clock each run
    if (state == RunState::Idle && prevState_ != RunState::Idle) {
        // #124: an IDLE entry follows a run's close sequence (or a stop/fault-clear),
        // and real hydraulics are still trailing — draindown pulses would land in the
        // first idle window and latch a nuisance UnexpectedFlow. Wait for the flow to
        // quiesce (capped) before arming; the window is baselined when the gate opens.
        draining_ = true;
        drain_.open(pulses, nowMs);
    }
    prevState_ = state;

    // --- RUNNING: no-flow after the grace window ---
    if (state == RunState::Running) {
        if (!muted_ &&
            (uint32_t)(nowMs - runStartMs_) >= cfg_.graceMs && rateGPM <= cfg_.minRunningGPM)
            return Fault::NoFlow;
        return Fault::None;
    }

    // --- IDLE: unexpected flow over a tumbling window ---
    // The window keeps sliding while muted (DEC-015 mutes verdicts, not tracking), so
    // un-muting starts from a fresh window instead of judging the whole muted span.
    if (state == RunState::Idle) {
        if (draining_) {
            if (!drain_.drained(pulses, nowMs)) return Fault::None;
            draining_          = false;   // drained (or capped): arm from a fresh baseline.
            idleBaseline_      = pulses;  // On a cap, flow that never decays now has one
            idleWindowStartMs_ = nowMs;   // full idleWindowMs to trip the check — a genuine
            return Fault::None;           // stuck-open/burst still latches.
        }
        if ((uint32_t)(nowMs - idleWindowStartMs_) >= cfg_.idleWindowMs) {
            const uint32_t delta = pulses - idleBaseline_;   // monotonic count; unsigned-safe
            idleBaseline_      = pulses;                      // slide the window either way, so a
            idleWindowStartMs_ = nowMs;                       // quiet window re-baselines cleanly
            if (!muted_ && delta > cfg_.idleFaultPulses) return Fault::UnexpectedFlow;
        }
        return Fault::None;
    }

    // --- transition states (Prep/Open/Start/Stop/Close/Settle) + Fault: neither check ---
    // Flow is legitimately ramping or trailing here; arming a check would false-positive.
    return Fault::None;
}

} // namespace tinkle
