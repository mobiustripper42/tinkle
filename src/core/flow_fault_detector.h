#pragma once
#include <stdint.h>
#include "run_controller.h"   // RunState + Fault — the shared vocabulary

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
// sequence, so faulting there would be a false positive.

namespace tinkle {

class FlowFaultDetector {
public:
    struct Config {
        // §15 seeds — bench-confirm.
        uint32_t graceMs        = 20000;   // FLOW_GRACE_S (20 s): pump spin-up + pipe fill
                                           // before the no-flow check arms.
        float    minRunningGPM  = 0.1f;    // at/below this during RUNNING (post-grace) reads
                                           // as no flow (clog, dead pump, valve never opened).
        uint32_t idleFaultPulses = 50;     // IDLE_FLOW_FAULT_PULSES ("tune", §15): pulses over
                                           // one idle window above which flow is "unexpected".
        uint32_t idleWindowMs   = 5000;    // tumbling window for the idle-flow accumulation.
    };

    explicit FlowFaultDetector(const Config& cfg) : cfg_(cfg) {}

    // Seed the window origins at boot (call from setup(), like FlowMonitor::begin()).
    // prevState_ starts at Idle, so the first Idle tick is NOT an edge and never
    // re-baselines — without this, a setup() longer than idleWindowMs (a slow WiFi
    // join) would evaluate a pseudo-window spanning all of boot and could latch a
    // nuisance UnexpectedFlow from pulses counted since FlowSensor::begin().
    void begin(uint32_t pulses, uint32_t nowMs) {
        idleBaseline_      = pulses;
        idleWindowStartMs_ = nowMs;
        runStartMs_        = nowMs;
    }

    // Call every loop tick. `pulses` is FlowMonitor's monotonic cumulative count (for the
    // idle check); `rateGPM` is its rolling rate (for the no-flow check). Returns the fault
    // to raise this tick, or Fault::None. Idempotent against an already-latched run: once a
    // fault sends the state to Fault, neither check arms, so it won't re-raise.
    Fault update(RunState state, float rateGPM, uint32_t pulses, uint32_t nowMs);

private:
    Config   cfg_;
    RunState prevState_         = RunState::Idle;
    uint32_t runStartMs_        = 0;   // when RUNNING was (re)entered — grace clock origin
    uint32_t idleBaseline_      = 0;   // pulse count at the start of the current idle window
    uint32_t idleWindowStartMs_ = 0;
};

} // namespace tinkle
