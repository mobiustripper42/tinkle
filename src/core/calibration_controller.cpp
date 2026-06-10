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

    // Finish mid-run freezes the tally NOW and unwinds — pulses that trail during the
    // close travel go uncounted, biasing K slightly low. The SPA flow finishes after
    // the bounded run ends (Awaiting), where the tally includes the trailing flow.
    if (phase_ == Phase::Running) {
        tally_ = pulses - baseline_;
        rc_.stop(nowMs);
    }
    phase_ = Phase::Idle;                        // the calibration ends either way

    // Sanity bounds (§7). The volume compare is written so NaN fails it; a zero tally
    // yields K = 0, which the range check rejects — no divide-by-zero path to a write.
    if (!(measuredGallons >= cfg_.minGallons)) {
        rc_.raiseFault(Fault::CalRange, nowMs);
        return FinishResult::Rejected;
    }
    const float k = (float)tally_ / measuredGallons;
    if (k < cfg_.minK || k > cfg_.maxK) {
        rc_.raiseFault(Fault::CalRange, nowMs);
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
