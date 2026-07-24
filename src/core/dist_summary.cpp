#include "dist_summary.h"

namespace tinkle {

namespace {

// One cycle spans all live zones back-to-back + per-run overhead, rounded up to a whole minute —
// the same span the plan derivation uses (scheduler.cpp computeDistributedPlan). A cycle's runs
// have all finished by slotStart + this, so that's both the match window's upper edge and the
// "cycle has elapsed" boundary. Never 0 (a valid plan has runLenSec > 0 and zoneCount >= 1).
uint16_t cycleSpanMin(const DistributedPlan& plan, uint8_t zoneCount) {
    const uint32_t spanSec = (uint32_t)zoneCount * (plan.runLenSec + DIST_RUN_OVERHEAD_SEC);
    const uint16_t m = (uint16_t)((spanSec + 59u) / 60u);
    return m ? m : 1;
}

// The [start, end) minute-of-day window for cycle c. A run for this cycle lands inside it; once
// nowMin reaches `end` the cycle has fully elapsed. Bounded to the day so the last cycle can't
// run past midnight.
void slotWindow(const DistributedPlan& plan, uint16_t spanMin, uint8_t c,
                uint16_t& start, uint16_t& end) {
    start = plan.cycleStartMin[c];
    uint32_t e = (uint32_t)start + spanMin;
    end = (uint16_t)(e > 1440u ? 1440u : e);
}

// True if `e` is a run for (zone z, cycle c) today — any result. clockWasValid gates: a pre-NTP
// entry has a meaningless epoch and can't be located in a day (matches the publisher's own gate).
bool entryInSlot(const RunEntry& e, uint8_t z, uint32_t dayOrdinal, uint16_t start, uint16_t end) {
    if (!e.clockWasValid || e.zoneIndex != z)          return false;
    if (e.startEpoch / 86400u != dayOrdinal)           return false;
    const uint16_t minOfDay = (uint16_t)((e.startEpoch / 60u) % 1440u);
    return minOfDay >= start && minOfDay < end;
}

} // namespace

DistDaySummary computeDistSummary(const RunLog& log, const DistributedPlan& plan,
                                  uint8_t zoneCount, uint32_t dayOrdinal, uint16_t nowMin) {
    DistDaySummary s;
    if (!plan.valid || zoneCount == 0) return s;       // active=false; caller renders nothing
    if (zoneCount > ValveConfig::MAX_ZONES) zoneCount = ValveConfig::MAX_ZONES;

    s.active    = true;
    s.cycles    = plan.cycles;
    s.zoneCount = zoneCount;

    const uint16_t spanMin = cycleSpanMin(plan, zoneCount);
    const uint8_t  entries = log.count();

    for (uint8_t z = 0; z < zoneCount; ++z) {
        DistZoneSummary& zs = s.zones[z];
        for (uint8_t c = 0; c < plan.cycles; ++c) {
            uint16_t start, end;
            slotWindow(plan, spanMin, c, start, end);

            // Only judge a cycle once it has FULLY elapsed (nowMin >= end) — the same boundary
            // nextMissedCycle uses. A cycle that has merely *started* may still be mid-execution
            // (a zone waiting its back-to-back turn), so counting it "planned" while its run
            // hasn't finished would show completed < plannedByNow and light a false "behind" ⚠ for
            // most of every cycle's ~span. Gating both counts on `end` keeps "M/N" and the ⚠
            // honest: N grows only as cycles close, and completed < planned means a genuine miss.
            if (nowMin < end) continue;
            ++zs.plannedByNow;

            // A cycle counts as completed only on a Completed run — a flow-faulted run fired but
            // didn't deliver, so it correctly reads as short (completed < planned), not done.
            for (uint8_t i = 0; i < entries; ++i) {
                const RunEntry e = log.at(i);
                if (e.result == RunResult::Completed && entryInSlot(e, z, dayOrdinal, start, end)) {
                    ++zs.completed;
                    zs.centigallons = (uint16_t)(zs.centigallons + e.centigallons);
                    break;                             // one completed run per cycle
                }
            }
        }
    }
    return s;
}

bool nextMissedCycle(const RunLog& log, const DistributedPlan& plan, uint8_t zoneCount,
                     uint32_t dayOrdinal, uint16_t nowMin, uint8_t missedFaultCode,
                     RunEntry& out) {
    if (!plan.valid || zoneCount == 0) return false;
    if (zoneCount > ValveConfig::MAX_ZONES) zoneCount = ValveConfig::MAX_ZONES;

    const uint16_t spanMin = cycleSpanMin(plan, zoneCount);
    const uint8_t  entries = log.count();

    for (uint8_t c = 0; c < plan.cycles; ++c) {
        uint16_t start, end;
        slotWindow(plan, spanMin, c, start, end);
        if (nowMin < end) break;                       // not elapsed yet — later cycles even less

        for (uint8_t z = 0; z < zoneCount; ++z) {
            bool logged = false;
            for (uint8_t i = 0; i < entries && !logged; ++i)
                logged = entryInSlot(log.at(i), z, dayOrdinal, start, end);
            if (logged) continue;                      // ran, faulted, or already marked missed

            // Missed: no run of any result in this elapsed slot. Stamp a Faulted entry at the
            // cycle's local start so it lands back in this slot (dedup) and telemeters like a run.
            // Every field set explicitly — no aggregate-init (illegal under gnu++11, see env:native).
            out.startEpoch    = dayOrdinal * 86400u + (uint32_t)start * 60u;
            out.zoneIndex     = z;
            out.durationSec   = 0;
            out.centigallons  = 0;
            out.fertigate     = false;
            out.result        = RunResult::Faulted;
            out.clockWasValid = true;
            out.faultCode     = missedFaultCode;
            return true;
        }
    }
    return false;
}

} // namespace tinkle
