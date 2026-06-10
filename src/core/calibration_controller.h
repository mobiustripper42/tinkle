#pragma once
#include <stdint.h>
#include "run_controller.h"
#include "persistence.h"
#include "flow_monitor.h"

// CalibrationController — guided flow-sensor calibration (firmware spec §7, #36).
//
// Platform-independent (src/core): the calibration state machine + K math live here,
// host-tested with a fake clock/GPIO/store. It never touches an actuator — the
// calibration run is requested and stopped through RunController (the sole commander),
// exactly like a scheduled or manual run.
//
// FLOW: start(zone) requests a bounded run (its own max-runtime, Config::runSec) with
// the diverter plain, and baselines its OWN copy of the cumulative pulse counter — it
// does not share FlowMonitor's per-run accumulation, so main's RUNNING-edge re-baseline
// can't disturb the tally. The user collects the output in a known container; finish
// (measuredGallons) computes K = pulsesCounted / measuredGallons, range-checks it, and
// on success writes K to NVS (Persistence) and into the live FlowMonitor.
//
// TALLY LIFECYCLE: the tally tracks the live counter from start() until the run reaches
// SETTLE / IDLE / FAULT, then freezes. Trailing pulses while the zone valve closes
// (STOP_PUMP -> CLOSE_ZONE) are still water in the user's container, so they count; by
// SETTLE the zone is closed and flow is done. Freezing at SETTLE (not IDLE) matters
// because a queued run chains SETTLE -> next run without visiting IDLE — without the
// freeze, a second run's water would inflate the tally.
//
// REJECTION = FAULT_CAL_RANGE (§14): an absurd measured volume or an out-of-range K is
// a failed calibration, not a soft retry — raiseFault(CalRange) latches and slams the
// safe state, and K keeps its prior value. The Phase 4 endpoint range-validates input
// before calling finish(), so a latched CalRange means the measurement itself is
// untrustworthy, which is exactly when the operator should be made to look.

namespace tinkle {

class CalibrationController {
public:
    struct Config {
        // Bounded calibration run — its own ceiling, independent of swMaxRuntimeSec
        // (well under it: a bucket test at a few GPM takes 1-2 min). TINKLE_SIM
        // ([env:esp32_sim]) shortens it so a Wokwi calibration finishes in seconds.
#ifdef TINKLE_SIM
        uint32_t runSec = 10;
#else
        uint32_t runSec = 120;          // CAL_RUN_SEC (§15 seed)
#endif
        // Sanity bounds. K bounds are ~10x around the 450 datasheet seed; minGallons
        // rejects a volume too small to calibrate against (§15 seeds, tune on bench).
        float minGallons = 0.25f;
        float minK       = 50.0f;
        float maxK       = 5000.0f;
    };

    // Idle: no calibration. Running: cal run requested/active, tally live.
    // Awaiting: run finished (completed or stopped), tally frozen, finish() pending.
    enum class Phase : uint8_t { Idle, Running, Awaiting };

    enum class FinishResult : uint8_t {
        Ok,              // K computed, persisted, applied to FlowMonitor
        NotCalibrating,  // no calibration in progress — no fault raised
        Rejected         // sanity bounds failed — FAULT_CAL_RANGE raised, K unchanged
    };

    CalibrationController(RunController& rc, Persistence& store, FlowMonitor& flow,
                          const Config& cfg);

    // Begin a calibration run on zoneIndex: baseline the pulse tally and request the
    // bounded run (diverter plain). Requires both this controller and RunController to
    // be idle — a calibration never queues behind other runs. Returns false if refused
    // (already calibrating, run active, latched fault, bad zone).
    bool start(uint8_t zoneIndex, uint32_t pulses, uint32_t nowMs);

    // Complete the calibration: K = pulsesCounted / measuredGallons. On success the K
    // is written to NVS and the live FlowMonitor, and any still-active cal run stops
    // (graceful unwind). Out-of-bounds volume or K raises FAULT_CAL_RANGE (§14) and
    // leaves K untouched. Either way the calibration ends (Phase::Idle).
    FinishResult finish(float measuredGallons, uint32_t pulses, uint32_t nowMs);

    // Abort without judging: stop the run if still active, discard the tally, no
    // fault, no K write. (Phase 4 wires this to the calibration screen's cancel.)
    void cancel(uint32_t nowMs);

    // Track the calibration run's lifecycle against the run state machine. Call every
    // loop tick (after RunController::tick). Freezes the tally at SETTLE/IDLE entry;
    // a FAULT during the run aborts the calibration (the fault owns the story).
    void tick(RunState runState, uint32_t pulses, uint32_t nowMs);

    Phase    phase() const         { return phase_; }
    bool     active() const        { return phase_ != Phase::Idle; }
    uint32_t pulsesCounted() const { return tally_; }
    float    lastK() const         { return lastK_; }   // 0 until a calibration succeeds

private:
    RunController& rc_;
    Persistence&   store_;
    FlowMonitor&   flow_;
    Config         cfg_;

    Phase    phase_    = Phase::Idle;
    uint32_t baseline_ = 0;   // cumulative pulse count at start()
    uint32_t tally_    = 0;   // pulses delivered this calibration (frozen at Awaiting)
    float    lastK_    = 0.0f;
};

} // namespace tinkle
