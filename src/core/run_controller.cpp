#include "run_controller.h"

namespace tinkle {

RunController::RunController(ValveDriver& valve, const RunConfig& cfg)
    : valve_(valve), cfg_(cfg) {}

// Push the current run onto the history ring (DEC-018). Field assignment, not brace-init:
// RunEntry carries default member initializers, so `RunEntry{...}` is ill-formed at the
// firmware's -std=gnu++11 (arduino-esp32 2.0.x). centigallons + startEpoch come from the
// pending metrics main pushed in (noteRunVolume / noteRunStart); the rest is RunController's
// own. One helper keeps the SETTLE and fault sites in sync. Marks the log dirty for the
// debounced NVS write in main.
void RunController::logRun(RunResult result, Fault fault) {
    RunEntry e;
    e.startEpoch    = pendingStartEpoch_;
    e.zoneIndex     = current_.zoneIndex;
    e.durationSec   = current_.durationSec > 0xFFFFu ? 0xFFFFu
                                                     : (uint16_t)current_.durationSec;
    e.centigallons  = pendingCentigallons_;
    e.fertigate     = current_.fertigate;
    e.result        = result;
    e.clockWasValid = pendingClockValid_;
    e.faultCode     = (uint8_t)fault;
    runlog_.push(e);
    runLogDirty_ = true;
}

void RunController::resetRunMetrics() {
    pendingStartEpoch_   = 0;
    pendingClockValid_   = false;
    pendingCentigallons_ = 0;
    pendingRunFault_     = Fault::None;   // DEC-023: an abort never leaks across runs
}

void RunController::noteRunStart(uint32_t startEpoch, bool clockValid) {
    // Pre-2025 epoch-sanity guard (DEC-018): a pre-NTP free-run epoch is stored but flagged
    // invalid, so the SPA renders it relative-to-uptime rather than a bogus ~1970 wall-clock.
    pendingStartEpoch_ = startEpoch;
    pendingClockValid_ = clockValid && startEpoch >= RUNLOG_MIN_VALID_EPOCH;
}

void RunController::begin(uint32_t nowMs) {
    valve_.begin();             // configure every actuator pin as an output + safe levels
    valve_.safeState(nowMs);    // then force the fail-dry rest state (pump off, all FETs off)
    state_  = RunState::Idle;
    fault_  = Fault::None;
    qHead_ = qCount_ = 0;
    stopping_ = false;
    resetRunMetrics();          // the ring itself is rehydrated from NVS by main, not here
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
    if (otaActive_) return false;   // #126: never start (or queue) a run mid-flash
    if (req.zoneIndex >= valve_.zoneCount() || req.durationSec == 0) return false;

    if (state_ == RunState::Idle) {
        current_ = req;
        stopping_ = false;
        resetRunMetrics();
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
    resetRunMetrics();
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
    if (code == Fault::ValveRest) return;           // DEC-014 is log-only (FaultManager::note)
                                                    // — a maintenance flag must never halt
                                                    // irrigation. Structural, not convention.
    if (code == Fault::Watchdog) return;            // DEC-023: non-latching — route through
                                                    // abortRun(). The relay already cut the
                                                    // pump; a latch here only costs watering.
    if (code >= Fault::Count) return;               // sentinel/garbage is a caller bug
    if (state_ == RunState::Fault) return;          // first fault wins
    valve_.safeState(nowMs);                        // pump off -> zones de-energized -> diverter plain
    fault_ = code;
    qHead_ = qCount_ = 0;
    logRun(RunResult::Faulted, fault_);             // faulted runs land in the ring too (DEC-018)
    state_ = RunState::Fault;
    stateStartMs_ = nowMs;
}

bool RunController::abortRun(Fault code, uint32_t nowMs) {
    // DEC-023: end the CURRENT run for a non-latching fault (Watchdog) — unwind
    // through the normal StopPump→CloseZone→Settle path, log it Faulted, and
    // PRESERVE the queue. The hardware backstop already de-powered the pump; the
    // rest of the schedule must not pay for one run's trip.
    if (code == Fault::None || code >= Fault::Count) return false;
    if (state_ == RunState::Idle || state_ == RunState::Fault) return false;
    if (state_ == RunState::Settle) return false;   // already logged at Settle entry;
                                                    // water stopped — nothing to abort
    if (pendingRunFault_ != Fault::None) return false;   // this run's abort is in flight
    pendingRunFault_ = code;
    // From the shutdown tail (StopPump/CloseZone), stay on the unwind already in
    // progress — re-entering StopPump would restart the close travel.
    if (state_ != RunState::StopPump && state_ != RunState::CloseZone)
        enter(RunState::StopPump, nowMs);
    return true;
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
            // §4 step 2 moved to the PrepDiverter→OpenZone transition (DEC-023):
            // the gate now HOLDS the run until the trip line releases instead of
            // killing it here — by the time this entry runs, the line is clear.
            valve_.openZone(current_.zoneIndex, nowMs);
            break;
        case RunState::StartPump:   valve_.pumpOn();                            break;
        case RunState::Running:     runStartMs_ = nowMs;                        break;
        case RunState::StopPump:    valve_.pumpOff();                           break;
        case RunState::CloseZone:   valve_.closeZone(current_.zoneIndex, nowMs);break;
        case RunState::Settle:
            // Push the run onto the history ring (§4 step 7 / DEC-018). A DEC-023
            // abort (non-latching Watchdog) unwound through the normal path and
            // logs Faulted here — one log site for every outcome.
            if (pendingRunFault_ != Fault::None) {
                logRun(RunResult::Faulted, pendingRunFault_);
                pendingRunFault_ = Fault::None;
            } else {
                logRun(stopping_ ? RunResult::Stopped : RunResult::Completed, Fault::None);
            }
            // Return the diverter to plain rest (§5/§14, DEC-011/013/021) — only when the
            // queue is empty; a chained run sets its legs in PrepDiverter, so returning
            // between queued runs is wasted travel and risks overlapping leg commands. The
            // diverterFert() guard keeps a plain run's SETTLE travel-free.
            if (qCount_ == 0 && valve_.diverterFert()) valve_.setDiverter(false, nowMs);
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
            if (valve_.diverterBusy()) return;
            // §4 step 2 (DEC-023): while the backstop reports tripped, HOLD here —
            // nothing is energized yet, and the lockout self-releases ≤ 2 s after
            // the previous run's heartbeat quiets. Wait-not-kill: the old abort
            // turned a stale trip read into a dead schedule. A line stuck asserted
            // past wdWaitMs skips THIS run only (logged Faulted via the normal
            // unwind) and lets the queue advance.
            if (watchdogTripped_) {
                if ((uint32_t)(nowMs - stateStartMs_) >= cfg_.wdWaitMs) {
                    pendingRunFault_ = Fault::Watchdog;
                    enter(RunState::StopPump, nowMs);   // unwind a never-opened path
                }
                return;
            }
            enter(RunState::OpenZone, nowMs);
            return;

        case RunState::OpenZone:
            // Wait out the zone valve's travel so the pump never loads against a
            // closed/mid-travel valve (§4 step 2). (The pre-open gate held in
            // PrepDiverter; a trip from here on arrives via abortRun(), DEC-023.)
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
