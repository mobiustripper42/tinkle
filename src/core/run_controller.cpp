#include "run_controller.h"

namespace tinkle {

RunController::RunController(ValveDriver& valve, const RunConfig& cfg)
    : valve_(valve), cfg_(cfg) {}

void RunController::begin(uint32_t nowMs) {
    valve_.safeState(nowMs);
    state_  = RunState::Idle;
    fault_  = Fault::None;
    qHead_ = qCount_ = 0;
    stopping_ = false;
}

uint32_t RunController::effectiveDurationMs() const {
    // The software ceiling (§15) caps any single run, so a misconfigured or
    // oversized duration can never outrun the firmware's own stop. A zero ceiling
    // is treated as "no software cap" (a 0 ms clamp would silently water for zero
    // seconds); the ATtiny hard ceiling is still the backstop.
    uint32_t sec = current_.durationSec;
    if (cfg_.swMaxRuntimeSec > 0 && sec > cfg_.swMaxRuntimeSec) sec = cfg_.swMaxRuntimeSec;
    return sec * 1000u;
}

bool RunController::requestRun(const RunRequest& req, uint32_t nowMs) {
    if (state_ == RunState::Fault) return false;
    if (req.zoneIndex >= valve_.zoneCount() || req.durationSec == 0) return false;

    if (state_ == RunState::Idle) {
        current_ = req;
        stopping_ = false;
        enter(RunState::PrepDiverter, nowMs);
        return true;
    }
    // A run is active — queue it.
    if (qCount_ >= RunConfig::MAX_QUEUE) return false;
    queue_[(qHead_ + qCount_) % RunConfig::MAX_QUEUE] = req;
    ++qCount_;
    return true;
}

bool RunController::startNext(uint32_t nowMs) {
    if (qCount_ == 0) return false;
    current_ = queue_[qHead_];
    qHead_ = (qHead_ + 1) % RunConfig::MAX_QUEUE;
    --qCount_;
    stopping_ = false;
    enter(RunState::PrepDiverter, nowMs);
    return true;
}

void RunController::stop(uint32_t nowMs) {
    // Graceful cancel from any active step. Drop the queue and unwind through the
    // normal shutdown path. Closing an already-closed zone/master is a harmless
    // no-op, so this is safe to enter from PREP/OPEN_* as well.
    if (state_ == RunState::Idle || state_ == RunState::Fault) return;
    qHead_ = qCount_ = 0;
    stopping_ = true;
    enter(RunState::StopPump, nowMs);
}

void RunController::raiseFault(Fault code, uint32_t nowMs) {
    if (code == Fault::None) return;                // "no fault" is a caller bug, not a latch
    if (state_ == RunState::Fault) return;          // first fault wins
    valve_.safeState(nowMs);                        // pump off -> zones closed -> master closed
    fault_ = code;
    qHead_ = qCount_ = 0;
    lastRun_ = RunSummary{ RunSummary::Result::Faulted,
                           current_.zoneIndex, current_.durationSec,
                           current_.fertigate, fault_ };
    state_ = RunState::Fault;
    stateStartMs_ = nowMs;
}

bool RunController::clearFault() {
    if (state_ != RunState::Fault) return false;
    fault_ = Fault::None;
    state_ = RunState::Idle;
    return true;
}

void RunController::enter(RunState s, uint32_t nowMs) {
    state_ = s;
    stateStartMs_ = nowMs;
    switch (s) {
        case RunState::PrepDiverter:
            // Only spend the 6 s travel when the position actually needs to
            // change (§6). First run (position unknown) always travels.
            if (!(valve_.diverterKnown() &&
                  valve_.diverterThrough() == current_.fertigate)) {
                valve_.setDiverter(current_.fertigate, nowMs);
            }
            break;
        case RunState::OpenMaster:
            // §4 step 2: never open water if the hardware backstop reports tripped.
            if (watchdogTripped_) { raiseFault(Fault::Watchdog, nowMs); return; }
            valve_.masterOpen();
            // OpenMaster -> OpenZone -> StartPump are instantaneous (advance one
            // tick each, ≤~30 ms total). Zero inter-state dwell is intentional for
            // now; bench-confirm whether the NC master + latch want a settle beat
            // before the pump loads (§15 — physical constants confirmed on parts).
            break;
        case RunState::OpenZone:    valve_.pulseOpen(current_.zoneIndex, nowMs); break;
        case RunState::StartPump:   valve_.pumpOn();                           break;
        case RunState::Running:     runStartMs_ = nowMs;                       break;
        case RunState::StopPump:    valve_.pumpOff();                          break;
        case RunState::CloseZone:   valve_.pulseClose(current_.zoneIndex, nowMs); break;
        case RunState::CloseMaster: valve_.masterClose();                      break;
        case RunState::Settle:
            // Log the run outcome (§4 step 9). Diverter left as-is.
            lastRun_ = RunSummary{
                stopping_ ? RunSummary::Result::Stopped : RunSummary::Result::Completed,
                current_.zoneIndex, current_.durationSec, current_.fertigate, Fault::None };
            break;
        default: break;
    }
}

void RunController::tick(uint32_t nowMs) {
    valve_.tick(nowMs);                              // we own the actuator timers

    switch (state_) {
        case RunState::Idle:
        case RunState::Fault:
            return;                                  // nothing advances on its own

        case RunState::PrepDiverter:
            if (!valve_.diverterBusy()) enter(RunState::OpenMaster, nowMs);
            return;

        case RunState::OpenMaster:                   // instantaneous; advance next tick
            enter(RunState::OpenZone, nowMs);
            return;

        case RunState::OpenZone:
            if (!valve_.zoneBusy(current_.zoneIndex)) enter(RunState::StartPump, nowMs);
            return;

        case RunState::StartPump:
            enter(RunState::Running, nowMs);
            return;

        case RunState::Running:
            if ((uint32_t)(nowMs - runStartMs_) >= effectiveDurationMs())
                enter(RunState::StopPump, nowMs);
            return;

        case RunState::StopPump:
            enter(RunState::CloseZone, nowMs);
            return;

        case RunState::CloseZone:
            if (!valve_.zoneBusy(current_.zoneIndex)) enter(RunState::CloseMaster, nowMs);
            return;

        case RunState::CloseMaster:
            enter(RunState::Settle, nowMs);
            return;

        case RunState::Settle:
            if ((uint32_t)(nowMs - stateStartMs_) >= cfg_.settleMs) {
                if (!startNext(nowMs)) { state_ = RunState::Idle; stateStartMs_ = nowMs; }
            }
            return;
    }
}

int RunController::activeZone() const {
    switch (state_) {
        case RunState::Idle:
        case RunState::Fault:
        case RunState::Settle:
            return -1;
        default:
            return (int)current_.zoneIndex;
    }
}

uint32_t RunController::remainingSec(uint32_t nowMs) const {
    if (state_ != RunState::Running) return 0;
    uint32_t elapsed = (uint32_t)(nowMs - runStartMs_);
    uint32_t total   = effectiveDurationMs();
    if (elapsed >= total) return 0;
    return (total - elapsed + 999u) / 1000u;         // round up the displayed seconds
}

} // namespace tinkle
