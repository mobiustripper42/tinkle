#pragma once
#include <stdint.h>
#include "valve_driver.h"

// RunController — the run state machine (firmware spec §4) and the ONLY module
// allowed to command ValveDriver. Everything else (scheduler, buttons, web API)
// requests runs through it, so the whole actuation sequence lives in one
// auditable place.
//
// Platform-independent (src/core): driven by an explicit millis() value passed
// to tick(), host-tested with a fake clock + fake GPIO + injected faults. It owns
// the injected ValveDriver and ticks it; nothing else touches the actuators.
//
// Decoupling: the fault detectors that don't exist yet (FlowMonitor §7,
// Watchdog §9) push faults in via raiseFault() rather than RunController pulling
// from them. That keeps Phase 1 free of Phase 3/5 dependencies while honoring
// §14: entering any fault commands the safe state (pump off -> zones de-energized
// -> diverter to plain) and latches until an explicit clear.

namespace tinkle {

// v1.4 sequence (DEC-012): there is no master valve, so no OPEN_MASTER/CLOSE_MASTER
// steps — the pump (the source) gates water. PREP_DIVERTER -> OPEN_ZONE -> START_PUMP
// -> RUNNING -> STOP_PUMP -> CLOSE_ZONE -> SETTLE -> IDLE (§4).
enum class RunState : uint8_t {
    Idle, PrepDiverter, OpenZone, StartPump, Running,
    StopPump, CloseZone, Settle, Fault
};

// §14 fault codes. None = no latched fault.
enum class Fault : uint8_t {
    None, NoFlow, UnexpectedFlow, Watchdog, CalRange, Clock
};

// A run request: which zone, how long, and whether to fertigate (diverter THROUGH).
struct RunRequest {
    uint8_t  zoneIndex   = 0;
    uint32_t durationSec = 0;
    bool     fertigate   = false;
};

// The enqueue seam. RunController implements it; callers that only *request* runs
// (Scheduler §13, the web API §10) depend on this narrow interface, not the whole state
// machine — and host tests drive a fake sink that records requests and can simulate a
// full-queue rejection. requestRun returns false when the run is refused (bad request,
// full queue, or a latched fault).
struct IRunSink {
    virtual bool requestRun(const RunRequest& req, uint32_t nowMs) = 0;
    virtual ~IRunSink() = default;
};

// Outcome of the most recent run, for logging / status (§4 step 9). Measured gallons
// live in FlowMonitor (§7, #34) — kept out of here to preserve the RunController <-> flow
// decoupling; main logs the per-run volume on the RUNNING edge. The Phase 4 status API can
// join the two if a single summary record is wanted.
struct RunSummary {
    enum class Result : uint8_t { None, Completed, Stopped, Faulted };
    Result   result      = Result::None;
    uint8_t  zone        = 0;
    uint32_t durationSec = 0;
    bool     fertigate   = false;
    Fault    fault       = Fault::None;
};

struct RunConfig {
    static constexpr uint8_t MAX_QUEUE = 4;
    uint16_t settleMs        = 1000;   // SETTLE dwell; also the inter-run gap (§4)
    uint32_t swMaxRuntimeSec = 1200;   // §15 software ceiling; caps any single run
};

class RunController : public IRunSink {
public:
    RunController(ValveDriver& valve, const RunConfig& cfg);

    // Boot entry: configure every actuator pin as an output, force the fail-dry
    // safe state via ValveDriver, and reset to IDLE. Call once at boot — nothing
    // else need touch ValveDriver::begin().
    void begin(uint32_t nowMs);

    // Request a run. Starts immediately if IDLE, else queues (up to MAX_QUEUE).
    // Rejects (returns false) on a bad zone/duration, a full queue, or while a
    // fault is latched.
    bool requestRun(const RunRequest& req, uint32_t nowMs) override;

    // Graceful cancel of the active run + queue: unwind STOP_PUMP -> CLOSE_ZONE
    // -> SETTLE -> IDLE from any active step (§4). No-op if idle.
    void stop(uint32_t nowMs);

    // Emergency fault entry (§14): immediately command the safe state and latch
    // the code. Called by FlowMonitor / Watchdog / etc. First fault wins.
    void raiseFault(Fault code, uint32_t nowMs);

    // Clear a latched fault and return to IDLE. Only valid in FAULT; the caller
    // is responsible for confirming the underlying condition is resolved (§14).
    // Returns false if not currently faulted.
    bool clearFault();

    // Latest ATtiny trip-line state, pushed in by the Watchdog module (§9). Used
    // as the §4 step-2 pre-open gate: if asserted when OPEN_ZONE is reached, the
    // run aborts to FAULT(Watchdog) instead of energizing the path. (A trip that
    // arrives mid-run still comes in via raiseFault().)
    void setWatchdogTripped(bool tripped) { watchdogTripped_ = tripped; }

    // Cooperative, non-blocking. Ticks the ValveDriver and advances at most one
    // state transition (gated by actuator timers / durations). Call every loop.
    void tick(uint32_t nowMs);

    // Status / display introspection.
    RunState          state()       const { return state_; }
    Fault             activeFault()  const { return fault_; }
    bool              isFaulted()    const { return state_ == RunState::Fault; }
    bool              isIdle()       const { return state_ == RunState::Idle; }
    int               activeZone()   const;            // -1 if no active run
    uint32_t          remainingSec(uint32_t nowMs) const; // 0 unless RUNNING
    uint8_t           queueDepth()   const { return qCount_; }
    const RunSummary& lastRun()      const { return lastRun_; }

private:
    void enter(RunState s, uint32_t nowMs);            // perform entry actions
    bool startNext(uint32_t nowMs);                    // dequeue -> begin a run
    uint32_t effectiveDurationMs() const;              // min(duration, swMax) in ms

    ValveDriver& valve_;
    RunConfig    cfg_;

    RunState   state_        = RunState::Idle;
    Fault      fault_        = Fault::None;
    RunRequest current_      = {};
    uint32_t   stateStartMs_ = 0;
    uint32_t   runStartMs_   = 0;
    bool       stopping_     = false;     // this run was cancelled, not completed
    bool       watchdogTripped_ = false;  // §4 step-2 pre-open gate

    RunRequest queue_[RunConfig::MAX_QUEUE] = {};
    uint8_t    qHead_  = 0;
    uint8_t    qCount_ = 0;

    RunSummary lastRun_ = {};
};

} // namespace tinkle
