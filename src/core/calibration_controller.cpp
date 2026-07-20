#include "calibration_controller.h"

namespace tinkle {

CalibrationController::CalibrationController(RunController& rc, Persistence& store,
                                             FlowMonitor& flow, const Config& cfg)
    : rc_(rc), store_(store), flow_(flow), cfg_(cfg) {}

bool CalibrationController::start(uint8_t zoneIndex, uint32_t pulses, uint32_t nowMs) {
    if (phase_ != Phase::Idle) return false;     // one calibration at a time
    if (!rc_.isIdle()) return false;             // never queue a cal behind other runs

    RunRequest req;
    req.zoneIndex   = zoneIndex;
    req.durationSec = cfg_.runSec;
    req.fertigate   = false;                     // diverter plain (§7)
    if (!rc_.requestRun(req, nowMs)) return false;   // bad zone / latched fault

    baseline_ = pulses;
    tally_    = 0;
    phase_    = Phase::Running;
    return true;
}

void CalibrationController::tick(RunState runState, uint32_t pulses, uint32_t nowMs) {
    (void)nowMs;
    if (phase_ != Phase::Running) return;

    tally_ = pulses - baseline_;                 // unsigned math survives counter wrap

    switch (runState) {
        case RunState::Fault:                    // run faulted -> calibration is void
            phase_ = Phase::Idle;
            tally_ = 0;
            break;
        case RunState::Settle:                   // zone closed, flow done: freeze the
        case RunState::Idle:                     // tally before a queued run can chain
            phase_ = Phase::Awaiting;
            break;
        default:                                 // run still moving water — tally live
            break;
    }
}

CalibrationController::FinishResult
CalibrationController::finish(float measuredGallons, uint32_t pulses, uint32_t nowMs) {
    if (phase_ == Phase::Idle) return FinishResult::NotCalibrating;

    // Whether THIS finish() owns the run in flight: only true when the cal's own bounded
    // run is still active (Phase::Running) — we stop it just below. In Phase::Awaiting the
    // cal run has already ended (rc_ went Settle->Idle), so any run rc_ is now moving is an
    // UNRELATED one (a due schedule entry or a manual request that started while the
    // operator was entering the number). A CalRange reject must never latch/log against
    // that run (#144) — the same phantom-log/latch family as #138, through a different door.
    const bool ownsRun = (phase_ == Phase::Running);

    // Finish mid-run freezes the tally NOW and unwinds — pulses that trail during the
    // close travel go uncounted, biasing K slightly low. The SPA flow finishes after
    // the bounded run ends (Awaiting), where the tally includes the trailing flow.
    if (phase_ == Phase::Running) {
        tally_ = pulses - baseline_;
        rc_.stop(nowMs);
    }
    phase_ = Phase::Idle;                        // the calibration ends either way

    // A bad calibration number is always Rejected (no K write). The CalRange latch is a
    // signal about the calibration, so raise it only when it can't harm an unrelated run:
    // this finish owns the run, or the controller is idle (the normal no-concurrent case).
    const bool safeToFault = ownsRun || rc_.isIdle();

    // Sanity bounds (§7). The volume compare is written so NaN fails it; a zero tally
    // yields K = 0, which the range check rejects — no divide-by-zero path to a write.
    if (!(measuredGallons >= cfg_.minGallons)) {
        if (safeToFault) rc_.raiseFault(Fault::CalRange, nowMs);
        return FinishResult::Rejected;
    }
    const float k = (float)tally_ / measuredGallons;
    if (k < cfg_.minK || k > cfg_.maxK) {
        if (safeToFault) rc_.raiseFault(Fault::CalRange, nowMs);
        return FinishResult::Rejected;
    }

    store_.setPulsesPerGallon(k);                // NVS — survives reboot (§8)
    flow_.setK(k);                               // live monitor uses it immediately
    lastK_ = k;
    return FinishResult::Ok;
}

void CalibrationController::cancel(uint32_t nowMs) {
    if (phase_ == Phase::Running) rc_.stop(nowMs);
    phase_ = Phase::Idle;
    tally_ = 0;
}

} // namespace tinkle
