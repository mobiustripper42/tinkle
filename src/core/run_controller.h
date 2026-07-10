#pragma once
#include <stdint.h>
#include "valve_driver.h"
#include "run_log.h"     // RunLog ring + RunEntry — the run-history store (DEC-018)

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

// §14 fault codes. None = no latched fault. Count is a sentinel (keep last) —
// FaultManager sizes its condition table off it. ValveRest (DEC-014) is
// log-only: it reaches the ring via FaultManager::note(), never raiseFault.
enum class Fault : uint8_t {
    None, NoFlow, UnexpectedFlow, Watchdog, CalRange, Clock, ValveRest, Count
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

// The run outcome (§4 step 9) is logged into the RunLog ring (DEC-018), not a standalone
// summary: each run pushes a RunEntry at SETTLE (and on a fault), and lastRun() is the ring
// HEAD — one source of truth. The two data RunController can't see for itself are pushed in,
// preserving the existing decoupling:
//   - the per-run volume (centigallons) lives in FlowMonitor (§7, #34) — main pushes it via
//     noteRunVolume() so this stays free of any flow dependency;
//   - the wall-clock start epoch lives in Clock (§13) — main pushes it via noteRunStart().
// Same push-in idiom as raiseFault(): detectors/time push toward RunController, never pulled.

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
    // Returns false if not currently faulted. FaultManager::requestClear() is the
    // only sanctioned caller — it owns the resolved-condition gate. Don't wire
    // new clear paths (e.g. /api/fault/clear) here directly.
    bool clearFault();

    // Live settings update (§10 POST /api/settings). Applies to the NEXT duration
    // check, so it also shortens an in-flight run. 0 = no software cap (§15 note).
    void setSwMaxRuntimeSec(uint32_t sec) { cfg_.swMaxRuntimeSec = sec; }

    // OTA inhibit (#126): while a firmware image is streaming to flash, no run may
    // START — scheduled or manual. The upload runs on the async_tcp task for tens
    // of seconds, and the request-time IDLE gate alone can't stop a scheduled run
    // coming due mid-flash; every run funnels through requestRun (the IRunSink
    // seam), so refusing here covers the scheduler and the API in one place. Set
    // by the OTA route on Update.begin() success, cleared on a failed/aborted
    // upload (a successful one ends in reboot). Deliberately RAM-only.
    void setOtaActive(bool active) { otaActive_ = active; }
    bool otaActive() const         { return otaActive_; }

    // Latest ATtiny trip-line state, pushed in by the Watchdog module (§9). Used
    // as the §4 step-2 pre-open gate: if asserted when OPEN_ZONE is reached, the
    // run aborts to FAULT(Watchdog) instead of energizing the path. (A trip that
    // arrives mid-run still comes in via raiseFault().)
    void setWatchdogTripped(bool tripped) { watchdogTripped_ = tripped; }

    // RunLog seams (DEC-018). main feeds the two external data the run entry needs:
    // noteRunStart() on the RUNNING-entry edge — the wall-clock start epoch and whether NTP
    // had synced; noteRunVolume() each tick while RUNNING — the live per-run centigallons,
    // so a fault mid-run still records a fresh volume, not a stale one. Both are stashed and
    // consumed when the entry is pushed at SETTLE / on a fault. The pre-2025 epoch-sanity
    // guard lives here: an implausible start epoch is logged with clockWasValid CLEAR, never
    // as a 1970 wall-clock (DEC-018).
    void noteRunStart(uint32_t startEpoch, bool clockValid);
    void noteRunVolume(uint16_t centigallons) { pendingCentigallons_ = centigallons; }

    // Cooperative, non-blocking. Ticks the ValveDriver and advances at most one
    // state transition (gated by actuator timers / durations). Call every loop.
    void tick(uint32_t nowMs);

    // Status / display introspection.
    RunState          state()       const { return state_; }
    Fault             activeFault()  const { return fault_; }
    bool              isFaulted()    const { return state_ == RunState::Fault; }
    bool              isIdle()       const { return state_ == RunState::Idle; }
    int               activeZone()   const;            // -1 if no active run
    // Fertigate flag of the ACTIVE run (false when none) — lastRun() only updates
    // at run end, so status displays must not read fert state from it mid-run.
    bool              activeFertigate() const { return activeZone() >= 0 && current_.fertigate; }
    uint32_t          remainingSec(uint32_t nowMs) const; // 0 unless RUNNING
    uint8_t           queueDepth()   const { return qCount_; }
    // The "last run" is the ring head (DEC-018) — one source of truth, persisted with it.
    const RunEntry&   lastRun()      const { return runlog_.head(); }

    // Run-history ring (DEC-018). runLog() is the read view (status head today, /api/history
    // next, #70); runLogRef() is the boot-only rehydrate seam — main fills it from the NVS
    // blob before the loop runs. runLogDirty() drives the debounced NVS write in main: set on
    // every push, cleared by markRunLogPersisted() once the blob is written.
    const RunLog& runLog()           const { return runlog_; }
    RunLog&       runLogRef()              { return runlog_; }   // boot rehydrate only
    bool          runLogDirty()      const { return runLogDirty_; }
    void          markRunLogPersisted()    { runLogDirty_ = false; }

private:
    void enter(RunState s, uint32_t nowMs);            // perform entry actions
    bool startNext(uint32_t nowMs);                    // dequeue -> begin a run
    uint32_t effectiveDurationMs() const;              // min(duration, swMax) in ms
    void logRun(RunResult result, Fault fault);        // push the current run onto the ring
    void resetRunMetrics();                            // clear pending epoch/volume at run start

    ValveDriver& valve_;
    RunConfig    cfg_;

    RunState   state_        = RunState::Idle;
    Fault      fault_        = Fault::None;
    RunRequest current_      = {};
    uint32_t   stateStartMs_ = 0;
    uint32_t   runStartMs_   = 0;
    bool       stopping_     = false;     // this run was cancelled, not completed
    bool       watchdogTripped_ = false;  // §4 step-2 pre-open gate
    // #126: written from the async_tcp task (under the web mutex), read by the loop.
    volatile bool otaActive_ = false;

    RunRequest queue_[RunConfig::MAX_QUEUE] = {};
    uint8_t    qHead_  = 0;
    uint8_t    qCount_ = 0;

    // Run history (DEC-018). The ring is the sole "last run" store; lastRun() reads its head.
    RunLog   runlog_;
    bool     runLogDirty_ = false;          // a push happened, awaiting the debounced NVS write
    // Metrics pushed in by main and consumed when an entry is logged. Reset at each run start
    // so a fault before RUNNING logs startEpoch=0 / volume=0 (renders relative, per DEC-018).
    uint32_t pendingStartEpoch_   = 0;
    bool     pendingClockValid_   = false;
    uint16_t pendingCentigallons_ = 0;
};

} // namespace tinkle
