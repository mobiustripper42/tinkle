#pragma once
#include <stdint.h>

// LoopMonitor — the cooperative loop's tick-budget instrument (firmware spec §2:
// "target loop tick <= 10 ms").
//
// Platform-independent (src/core): it holds no clock and never blocks. The caller
// measures each tick's duration (micros() on the ESP32) and feeds it in; this
// accumulates stats and flags overruns. Pure bookkeeping, host-testable — the loop
// scaffold itself lives in src/esp32/main.cpp and can't run on the host, but the
// budget accounting that makes the ≤10 ms target observable does.

namespace tinkle {

struct LoopStats {
    uint32_t ticks    = 0;   // ticks recorded since the last reset
    uint32_t overruns = 0;   // ticks whose duration exceeded the budget
    uint32_t lastUs   = 0;   // most recent tick duration
    uint32_t maxUs    = 0;   // worst tick duration since the last reset
};

class LoopMonitor {
public:
    explicit LoopMonitor(uint32_t budgetUs) : budgetUs_(budgetUs) {}

    // Record one tick's measured duration. Returns true if it overran the budget.
    bool record(uint32_t durUs);

    uint32_t         budgetUs()    const { return budgetUs_; }
    const LoopStats& stats()       const { return s_; }
    bool             lastOverran() const { return s_.lastUs > budgetUs_; }
    void             reset()             { s_ = LoopStats{}; }

private:
    uint32_t  budgetUs_;
    LoopStats s_{};
};

} // namespace tinkle
