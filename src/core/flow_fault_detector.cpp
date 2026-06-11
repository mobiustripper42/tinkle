#include "flow_fault_detector.h"

namespace tinkle {

Fault FlowFaultDetector::update(RunState state, float rateGPM, uint32_t pulses, uint32_t nowMs) {
    // --- edge bookkeeping (uses prevState_, so do it before the checks) ---
    if (state == RunState::Running && prevState_ != RunState::Running)
        runStartMs_ = nowMs;                         // restart the grace clock each run
    if (state == RunState::Idle && prevState_ != RunState::Idle) {
        idleBaseline_      = pulses;                 // baseline the idle-flow window
        idleWindowStartMs_ = nowMs;
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
