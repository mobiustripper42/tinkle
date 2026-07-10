#pragma once
#include <stdint.h>

// DrainGate — the #124 post-run drain-quiesce gate. With real hydraulics the flow
// meter keeps ticking after a run ends: drip zones drain down through the emitters
// and an NC valve seats slower under residual line pressure than the bench's
// square-wave stand-in (which stopped the instant the relay opened). Any check that
// arms on a state edge therefore judges the tail of the run, not the rest condition
// it means to watch — the 2-of-3-zones end-of-run FAULT_UNEXPECTED_FLOW / valve-rest
// false positives of #124.
//
// The gate watches the cumulative pulse count for a QUIET sub-window: at most
// `quietPulses` over `quietMs`. Any burst above that restarts the sub-window, so
// the gate opens only once flow has genuinely decayed to ~0 — robust to per-zone
// seat/draindown variance with no field-measured constant. A cap (`capMs`) bounds
// the wait so flow that NEVER decays still reaches the caller's check: the caller
// reads `capped()` to distinguish "drained clean" from "gave up waiting".
//
// Platform-independent, header-only; owned per-detector (FlowFaultDetector,
// ValveRestMonitor) rather than shared state — each has its own arming edge.

namespace tinkle {

class DrainGate {
public:
    struct Config {
        uint32_t quietMs;       // sub-window that must stay quiet to count as drained
        uint32_t quietPulses;   // max pulses tolerated inside one quiet sub-window
        uint32_t capMs;         // hard bound on the wait; after this, drained() is
                                // true with capped() set — flow never quieted
    };

    explicit DrainGate(const Config& cfg) : cfg_(cfg) {}

    // (Re)start the wait — call on the arming edge (post-run IDLE / SETTLE entry).
    void open(uint32_t pulses, uint32_t nowMs) {
        base_       = pulses;
        winStartMs_ = nowMs;
        openMs_     = nowMs;
        done_       = false;
        capped_     = false;
    }

    // Poll every tick with the monotonic cumulative pulse count. Returns true once
    // the flow has quiesced (or the cap expired); sticky until the next open().
    bool drained(uint32_t pulses, uint32_t nowMs) {
        if (done_) return true;
        if ((uint32_t)(pulses - base_) > cfg_.quietPulses) {
            base_       = pulses;          // still draining — restart the sub-window
            winStartMs_ = nowMs;
        } else if ((uint32_t)(nowMs - winStartMs_) >= cfg_.quietMs) {
            done_ = true;                  // a full quiet sub-window: drained clean
            return true;
        }
        if ((uint32_t)(nowMs - openMs_) >= cfg_.capMs) {
            done_   = true;                // never quieted — hand the verdict back
            capped_ = true;
            return true;
        }
        return false;
    }

    // True when drained() completed via the cap rather than a quiet sub-window.
    bool capped() const { return capped_; }

private:
    Config   cfg_;
    uint32_t base_       = 0;
    uint32_t winStartMs_ = 0;
    uint32_t openMs_     = 0;
    bool     done_       = true;   // begins "already drained" — boot idle needs no wait
    bool     capped_     = false;
};

} // namespace tinkle
