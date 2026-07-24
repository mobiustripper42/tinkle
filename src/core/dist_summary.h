#pragma once
#include <stdint.h>
#include "run_log.h"        // RunLog / RunEntry / RunResult
#include "scheduler.h"      // DistributedPlan, DIST_RUN_OVERHEAD_SEC
#include "valve_driver.h"   // ValveConfig::MAX_ZONES

// Distributed-Watering day summary + missed-cycle detection (#161).
//
// Distributed mode (DEC-024) plans the whole day at midnight and never looks back, so a
// latching fault mid-window — or a power-out — silently skips the rest of the day's cycles.
// This module reads the persisted RunLog against the derived plan and answers two questions,
// both PURE and host-tested (no clock, no I/O — the caller passes today's ordinal + minute):
//
//   1. computeDistSummary — the at-a-glance Home card: per zone, how many cycles should have
//      run by now, how many actually completed, and the metered gallons delivered. Every field
//      is exact (completed-cycle counts + metered centigallons); nothing is estimated.
//
//   2. nextMissedCycle — the oldest cycle+zone slot whose window has fully elapsed with NO run
//      logged (any result). Returned as a ready-to-log Faulted RunEntry (faultCode=MissedCycle)
//      that the caller pushes via RunController::logMissedCycle — so it telemeters to Grafana
//      like any faulted run. Idempotency is free: once logged, the entry sits in the same slot
//      window and a re-scan skips it, so reboots and re-runs never double-report a miss.
//
// A cycle that FIRED and flow-faulted already has its own faulted RunEntry, so it matches its
// slot and is never re-reported as missed — a flow fault is one fault, not two. Only a slot with
// zero logged runs is "missed" (the box was off, or a latch upstream suppressed the firing).

namespace tinkle {

// Per-zone tally of today's distributed watering. Exact — completed-cycle counts and metered
// volume, never an estimate.
struct DistZoneSummary {
    uint8_t  plannedByNow = 0;   // cycles whose start minute has passed (should have run by now)
    uint8_t  completed    = 0;   // cycles with a RunResult::Completed run for this zone today
    uint16_t centigallons = 0;   // metered volume delivered across those completed cycles
};

struct DistDaySummary {
    bool    active    = false;   // the distributed plan is valid (else the caller renders nothing)
    uint8_t cycles    = 0;       // plan cycles/zone/day (the "of N" in "M of N")
    uint8_t zoneCount = 0;
    DistZoneSummary zones[ValveConfig::MAX_ZONES] = {};
};

// Summarize today's distributed runs from the RunLog against the plan. `dayOrdinal` is the local
// calendar day (epoch/86400) and `nowMin` the local minute-of-day. active=false when the plan is
// invalid or zoneCount is 0.
DistDaySummary computeDistSummary(const RunLog& log, const DistributedPlan& plan,
                                  uint8_t zoneCount, uint32_t dayOrdinal, uint16_t nowMin);

// Find the OLDEST (cycle, zone) slot that has fully elapsed by `nowMin` and has NO RunLog entry
// of any result for today — a cycle that never ran and hasn't been recorded as missed. Fills
// `out` (result=Faulted, faultCode=missedFaultCode, zoneIndex, startEpoch = the slot's local
// start, clockWasValid=true, 0 duration / 0 volume) and returns true; false when nothing is
// missed. The caller logs `out`, which then occupies the slot so the next scan skips it.
bool nextMissedCycle(const RunLog& log, const DistributedPlan& plan, uint8_t zoneCount,
                     uint32_t dayOrdinal, uint16_t nowMin, uint8_t missedFaultCode,
                     RunEntry& out);

} // namespace tinkle
