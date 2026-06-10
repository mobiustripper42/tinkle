#include "run_controller.h"

namespace tinkle {

// Field assignment, not brace-aggregate-init: RunSummary carries default member
// initializers, so it's only an aggregate under C++14+. The firmware builds at
// -std=gnu++11 (arduino-esp32 2.0.x), where `RunSummary{a, b, ...}` is ill-formed.
// One helper keeps the two summary sites in sync and C++11-clean.
static RunSummary makeSummary(RunSummary::Result result, const RunRequest& req, Fault fault) {
    RunSummary s;
    s.result      = result;
    s.zone        = req.zoneIndex;
    s.durationSec = req.durationSec;
    s.fertigate   = req.fertigate;
    s.fault       = fault;
    return s;
}

RunController::RunController(ValveDriver& valve, const RunConfig& cfg)
    : valve_(valve), cfg_(cfg) {}

void RunController::begin(uint32_t nowMs) {
    valve_.begin();             // configure every actuator pin as an output + safe levels
    valve_.safeState(nowMs);    // then force the fail-dry rest state (pump off, all FETs off)
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
    // normal shutdown path. Closing an already-closed zone is a harmless no-op, so
    // this is safe to enter from PREP/OPEN_ZONE as well.
    if (state_ == RunState::Idle || state_ == RunState::Fault) return;
    qHead_ = qCount_ = 0;
    stopping_ = true;
    enter(RunState::StopPump, nowMs);
}

void RunController::raiseFault(Fault code, uint32_t nowMs) {
    if (code == Fault::None) return;                // "no fault" is a caller bug, not a latch
    if (state_ == RunState::Fault) return;          // first fault wins
    valve_.safeState(nowMs);                        // pump off -> zones de-energized -> diverter plain
    fault_ = code;
    qHead_ = qCount_ = 0;
    lastRun_ = makeSummary(RunSummary::Result::Faulted, current_, fault_);
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
            // Only spend the travel when the leg state actually needs to change (§6).
            // The rest/boot state is plain (both legs de-energized), so a first plain
            // run skips travel; a first fert run travels.
            if (valve_.diverterFert() != current_.fertigate)
                valve_.setDiverter(current_.fertigate, nowMs);
            break;
        case RunState::OpenZone:
            // §4 step 2: never energize the path if the hardware backstop reports
            // tripped — abort before the zone opens and the pump (the source) starts.
            if (watchdogTripped_) { raiseFault(Fault::Watchdog, nowMs); return; }
            valve_.openZone(current_.zoneIndex, nowMs);
            break;
        case RunState::StartPump:   valve_.pumpOn();                            break;
        case RunState::Running:     runStartMs_ = nowMs;                        break;
        case RunState::StopPump:    valve_.pumpOff();                           break;
        case RunState::CloseZone:   valve_.closeZone(current_.zoneIndex, nowMs);break;
        case RunState::Settle:
            // Log the run outcome (§4 step 9). Diverter left as-is.
            lastRun_ = makeSummary(
                stopping_ ? RunSummary::Result::Stopped : RunSummary::Result::Completed,
                current_, Fault::None);
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
            if (!valve_.diverterBusy()) enter(RunState::OpenZone, nowMs);
            return;

        case RunState::OpenZone:
            // Wait out the zone valve's travel so the pump never loads against a
            // closed/mid-travel valve (§4 step 2). (The watchdog pre-open gate fired
            // on OpenZone entry; a trip mid-travel still arrives via raiseFault().)
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
            if (!valve_.zoneBusy(current_.zoneIndex)) enter(RunState::Settle, nowMs);
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
