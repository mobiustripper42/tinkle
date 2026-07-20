#pragma once
#include <stdint.h>
#include "run_controller.h"   // RunState + Fault — the shared vocabulary
#include "drain_gate.h"       // #124 post-run drain-quiesce gate

// FlowFaultDetector — the §7/§14 flow faults (#35). Decides WHEN flow is wrong;
// never actuates. Platform-independent (src/core): host-tested with injected pulse
// patterns + a fake clock.
//
// It reads FlowMonitor's output (rateGPM for no-flow, the cumulative pulse count for
// idle flow) plus the RunController state each tick and returns the Fault to raise —
// the caller routes a non-None result to RunController::raiseFault (the sole actuator
// commander), which commands the safe state and latches (§14). FlowMonitor stays pure
// measurement; this is the policy on top, mirroring how Watchdog (§9) will push faults.
//
// Window discipline (the #34/S6 carry-forward): the two checks are tied to RUNNING /
// IDLE *explicitly*, not to the per-run tally edge. Every transition state
// (PrepDiverter, OpenZone, StartPump, StopPump, CloseZone, Settle) is excluded —
// flow legitimately ramps up after the pump starts and trails off during the close
// sequence, so faulting there would be a false positive. The trailing-off doesn't
// stop at the IDLE edge either (#124): each IDLE entry passes through a DrainGate
// that holds the idle check unarmed until the draindown quiesces (capped), so a
// wet run's tail can't latch a nuisance UnexpectedFlow.

namespace tinkle {

class FlowFaultDetector {
public:
    struct Config {
        // §15 seeds — bench-confirm. TINKLE_SIM ([env:esp32_sim]) shortens the grace so the
        // no-flow fault is observable inside the 10 s sim run (DEFAULT_RUN_SEC) — at the real
        // 20 s grace a sim run would complete before the check ever armed. NOT hardware values.
#ifdef TINKLE_SIM
        uint32_t graceMs        = 3000;
#else
        uint32_t graceMs        = 20000;   // FLOW_GRACE_S (20 s): pump spin-up + pipe fill
                                           // before the no-flow check arms.
#endif
        float    minRunningGPM  = 0.1f;    // at/below this during RUNNING (post-grace) reads
                                           // as no flow (clog, dead pump, valve never opened).
        // IDLE_FLOW_FAULT_PULSES (§15): pulses over one idle window above which idle flow reads
        // as "unexpected". Real default ~1.0 GPM at the calibrated K≈1670 (139 pulses / 5 s), NOT
        // the old ~0.36 GPM (50 pulses): a decayed post-run draindown tail sits above 0.36 GPM for
        // minutes and false-latched E2 past the drain-gate cap (#141), while a welded pump relay
        // (~1.45 GPM ≈ 202 / 5 s) still trips well clear of the threshold. Valid for an AT-GRADE
        // catchment — pump-off ⇒ a stuck-open valve passes ~0 flow, so draindown is the only idle
        // flow to reject. K-referenced: recalibrating K far from 1670 shifts the GPM meaning, so
        // re-derive then. Exact value owes a wet-run draindown-envelope confirmation (#141).
        //
        // TINKLE_SIM keeps the old 50: the Wokwi fake-flow source is a fixed 15 Hz square wave
        // (≤ 75 pulses / 5 s window), so 139 is unreachable and E2 could never be provoked in the
        // sim tier (headless 02_idle_unexpected_flow.yaml + MANUAL.md walkthrough). 50 stays under
        // the 75-pulse ceiling so the fault still fires there — same reason its siblings split.
#ifdef TINKLE_SIM
        uint32_t idleFaultPulses = 50;
#else
        uint32_t idleFaultPulses = 139;
#endif
        uint32_t idleWindowMs   = 5000;    // tumbling window for the idle-flow accumulation.

        // #124 drain grace: on each IDLE entry the idle check waits for flow to
        // quiesce (≤ drainQuietPulses over drainQuietMs) before arming, capped at
        // drainCapMs so a genuine burst/stuck-open still faults — worst-case
        // detection latency is drainCapMs + idleWindowMs, with the pump off and the
        // safety relay dropped the whole time. §15 seeds — bench-confirm.
#ifdef TINKLE_SIM
        uint32_t drainQuietMs     = 1000;
        uint32_t drainQuietPulses = 2;
        uint32_t drainCapMs       = 5000;
#else
        uint32_t drainQuietMs     = 3000;  // DRAIN_QUIET_MS
        uint32_t drainQuietPulses = 2;     // DRAIN_QUIET_PULSES (~0.02 GPM at K≈1670)
        uint32_t drainCapMs       = 60000; // DRAIN_CAP_MS
#endif
    };

    explicit FlowFaultDetector(const Config& cfg)
        : cfg_(cfg),
          drain_({cfg.drainQuietMs, cfg.drainQuietPulses, cfg.drainCapMs}) {}

    // Seed the window origins at boot (call from setup(), like FlowMonitor::begin()).
    // prevState_ starts at Idle, so the first Idle tick is NOT an edge and never
    // re-baselines — without this, a setup() longer than idleWindowMs (a slow WiFi
    // join) would evaluate a pseudo-window spanning all of boot and could latch a
    // nuisance UnexpectedFlow from pulses counted since FlowSensor::begin().
    void begin(uint32_t pulses, uint32_t nowMs) {
        idleBaseline_      = pulses;
        idleWindowStartMs_ = nowMs;
        runStartMs_        = nowMs;
        draining_          = false;   // boot idle arms directly — nothing ran, nothing drains
    }

    // Call every loop tick. `pulses` is FlowMonitor's monotonic cumulative count (for the
    // idle check); `rateGPM` is its rolling rate (for the no-flow check). Returns the fault
    // to raise this tick, or Fault::None. Idempotent against an already-latched run: once a
    // fault sends the state to Fault, neither check arms, so it won't re-raise.
    Fault update(RunState state, float rateGPM, uint32_t pulses, uint32_t nowMs);

    // DEC-015 manual override (#57): while muted, update() returns None — faults are
    // muted, MEASUREMENT IS NOT (FlowMonitor keeps counting and reporting; the windows
    // here keep tracking, so un-muting needs no re-seed). Software-only by construction:
    // this gates only the two flow verdicts, never the Watchdog path or the pump gate.
    void setMuted(bool muted) { muted_ = muted; }
    bool muted() const        { return muted_; }

private:
    Config    cfg_;
    bool      muted_ = false;      // DEC-015: default = checks ON
    RunState  prevState_         = RunState::Idle;
    uint32_t  runStartMs_        = 0;  // when RUNNING was (re)entered — grace clock origin
    uint32_t  idleBaseline_      = 0;  // pulse count at the start of the current idle window
    uint32_t  idleWindowStartMs_ = 0;
    DrainGate drain_;                  // #124: gates idle-check arming on each IDLE entry
    bool      draining_ = false;       // waiting on drain_ before the idle window arms
};

} // namespace tinkle
