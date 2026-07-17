#include "scheduler.h"

namespace tinkle {

Scheduler::Scheduler(IRunSink& sink, Clock& clock, uint8_t zoneCount)
    : sink_(sink), clock_(clock), zoneCount_(zoneCount) {}

bool Scheduler::add(const ScheduleEntry& e) {
    if (count_ >= MAX_ENTRIES) return false;
    entries_[count_++] = e;
    return true;
}

void Scheduler::clear() {
    count_ = 0;
}

void Scheduler::tick(uint32_t nowMs) {
    if (!clock_.valid()) return;                         // no wall clock -> nothing schedulable
    const uint32_t curMin = clock_.epoch(nowMs) / 60u;   // absolute local minute
    // Forward-only: evaluate a given absolute minute at most once, ever. `<=` (not `==`)
    // matters because the DEC-009 hourly resync can nudge the epoch sub-second *backward*
    // across a minute boundary (06:00:00 -> 05:59:59); evaluating only strictly-newer
    // minutes drops that re-entry. A legitimate small backward correction lands on minutes
    // we already ran, so skipping them is correct, not a miss. (No free-run before the first
    // sync, so the baseline is always real NTP time — there's no wrong-base to "catch up" from.)
    if (evaledOnce_ && curMin <= lastEvalMin_) return;
    lastEvalMin_ = curMin;
    evaledOnce_  = true;
    evaluate(nowMs);
}

void Scheduler::evalNow(uint32_t nowMs) {
    if (!clock_.valid()) return;
    lastEvalMin_ = clock_.epoch(nowMs) / 60u;
    evaledOnce_  = true;
    evaluate(nowMs);                                     // re-evaluate the current minute on edit (§13)
}

bool Scheduler::resolveFert(const ScheduleEntry& e, uint32_t dayOrdinal, bool& consumesAutoSlot) const {
    consumesAutoSlot = false;
    switch (e.fertOverride) {
        case FertOverride::On:  return true;             // forced on, independent of the daily slot
        case FertOverride::Off: return false;            // forced off
        case FertOverride::Auto:
        default:
            // First auto run of the calendar day fertigates; the rest don't. The slot is
            // only *consumed* if the run enqueues (decided by the caller), so a rejected
            // run doesn't burn the day's fert.
            if (!autoFertUsed_ || autoFertDay_ != dayOrdinal) {
                consumesAutoSlot = true;
                return true;
            }
            return false;
    }
}

void Scheduler::evaluate(uint32_t nowMs) {
    const WallTime w    = clock_.wall(nowMs);
    const uint32_t day  = clock_.epoch(nowMs) / 86400u;  // local calendar-day ordinal

    // DEC-024: Distributed Watering replaces the entry schedule entirely (either/or). When
    // it's on AND schedulable, the fixed-time entries are dormant — never both in the same
    // minute. An enabled-but-INVALID config (corrupt blob / bad edit) is NOT schedulable, so
    // it must not silently water nothing: fall through to the fixed schedule (the "distributed
    // off" state). main.cpp logs the invalid config loudly at boot; the SPA blocks the save.
    if (dist_.enabled) {
        const DistributedPlan p = computeDistributedPlan(dist_, zoneCount_);
        if (p.valid) {
            evaluateDistributed(p, w.hour, w.minute, day, nowMs);
            return;
        }
    }

    const uint8_t  dayBit = static_cast<uint8_t>(1u << w.weekday);

    for (uint8_t i = 0; i < count_; ++i) {
        const ScheduleEntry& e = entries_[i];
        if (!e.enabled)                       continue;
        if (e.hour != w.hour || e.minute != w.minute) continue;
        if (!(e.daysMask & dayBit))           continue;   // not scheduled this weekday

        bool consumesAutoSlot = false;
        RunRequest req;
        req.zoneIndex   = e.zoneIndex;
        req.durationSec = e.durationSec;
        req.fertigate   = resolveFert(e, day, consumesAutoSlot);

        if (sink_.requestRun(req, nowMs)) {
            if (consumesAutoSlot) { autoFertUsed_ = true; autoFertDay_ = day; }
        } else {
            ++dropped_;                       // queue full / faulted -> drop + count (§13)
        }
    }
}

// Derive the plan (DEC-024). Pure + host-tested; the Scheduler and the SPA both call this
// so the preview never lies about what will actually fire. Invalid => nothing schedulable.
DistributedPlan computeDistributedPlan(const DistributedConfig& cfg, uint8_t zoneCount) {
    DistributedPlan p;                                   // valid=false, cycles=0
    if (!cfg.enabled || zoneCount == 0)                  return p;
    if (cfg.windowStartMin >= 1440 || cfg.windowEndMin > 1440) return p;   // minute-of-day bounds
    if (cfg.windowEndMin <= cfg.windowStartMin)          return p;
    if (cfg.perZoneMin < DIST_RUN_FLOOR_MIN)             return p;
    if (cfg.perZoneMin > DIST_MAX_PERZONE_MIN)           return p;   // sanity + u16 overflow guard

    const uint16_t winMin = (uint16_t)(cfg.windowEndMin - cfg.windowStartMin);

    // As many runs as fit above the floor, capped. Integer floor ⇒ every run ≥ the floor.
    uint16_t runs = (uint16_t)(cfg.perZoneMin / DIST_RUN_FLOOR_MIN);   // ≥1 (perZoneMin ≥ floor)
    if (runs > DIST_MAX_RUNS) runs = DIST_MAX_RUNS;

    const uint16_t runLenSec = (uint16_t)((uint32_t)cfg.perZoneMin * 60u / runs);

    // One cycle = all zones back-to-back + per-run overhead, rounded up to a whole minute
    // (the firing grid is per-minute).
    const uint32_t cycleSpanSec = (uint32_t)zoneCount * (runLenSec + DIST_RUN_OVERHEAD_SEC);
    const uint16_t cycleSpanMin = (uint16_t)((cycleSpanSec + 59u) / 60u);

    // Fit (bookended): all cycle spans must sit inside the window. Over-subscribed ⇒ invalid,
    // and the Scheduler emits nothing (the SPA blocks the save before it ever gets here).
    if ((uint32_t)runs * cycleSpanMin > winMin)          return p;

    p.runLenSec = runLenSec;
    p.cycles    = (uint8_t)runs;
    if (runs == 1) {
        p.cycleStartMin[0] = cfg.windowStartMin;
    } else {
        // Bookend: first cycle at the window start, last cycle ends at the window end. Even
        // gap between starts; the fit check guarantees gap ≥ cycleSpanMin (no overlap).
        const uint16_t gap = (uint16_t)((winMin - cycleSpanMin) / (runs - 1));
        for (uint8_t i = 0; i < runs; ++i)
            p.cycleStartMin[i] = (uint16_t)(cfg.windowStartMin + (uint32_t)i * gap);
    }
    p.valid = true;
    return p;
}

void Scheduler::evaluateDistributed(const DistributedPlan& p, uint8_t hour, uint8_t minute,
                                    uint32_t day, uint32_t nowMs) {
    // A cycle index must fit the u8 fired-mask; DIST_MAX_RUNS caps it well under 8.
    static_assert(DIST_MAX_RUNS <= 8, "distFiredMask_ (u8) holds one bit per cycle");
    // Each cycle fires one run per zone in a back-to-back burst; they must all fit the queue.
    static_assert(ValveConfig::MAX_ZONES <= RunConfig::MAX_QUEUE,
                  "a distributed cycle enqueues one run per zone at once");

    if (distFiredDay_ != day) { distFiredDay_ = day; distFiredMask_ = 0; }   // new day resets

    const uint16_t curMin = (uint16_t)(hour * 60 + minute);
    for (uint8_t i = 0; i < p.cycles; ++i) {
        if (p.cycleStartMin[i] != curMin)      continue;
        if (distFiredMask_ & (1u << i))         continue;   // fired today already (evalNow-safe)

        // Fire the whole cycle: one run per live zone, back-to-back. Fert rides the first
        // fertCount cycles (whole-cycle — every zone in those cycles fertigates, so no
        // per-zone imbalance). RunController queues them (depth = zoneCount, under MAX_QUEUE).
        //
        // DIAGNOSTIC (#147 session-23 / superseded by #151): fire the cycle Zone 3 -> 1 -> 2
        // instead of 1 -> 2 -> 3, to test whether the first-run flow spike on Zones 1 & 2
        // follows the zone or the firing position. Temporary hardcode; the configurable
        // per-zone order lands in #151, which reverts this. kFireOrder is a permutation of
        // 0..MAX_ZONES-1; indices >= the live zoneCount are skipped so each live zone still
        // fires exactly once, in this order.
        static constexpr uint8_t kFireOrder[ValveConfig::MAX_ZONES] = {2, 0, 1};
        static_assert(ValveConfig::MAX_ZONES == 3,
                      "kFireOrder is the hardcoded 3-zone diagnostic order (#151 makes it configurable)");
        const bool fert = (i < dist_.fertCount);
        for (uint8_t k = 0; k < ValveConfig::MAX_ZONES; ++k) {
            const uint8_t z = kFireOrder[k];
            if (z >= zoneCount_) continue;                   // fewer live zones than the full order
            RunRequest req;
            req.zoneIndex   = z;
            req.durationSec = p.runLenSec;
            req.fertigate   = fert;
            if (!sink_.requestRun(req, nowMs)) ++dropped_;   // full/faulted -> drop + count (§13)
        }
        distFiredMask_ |= (uint8_t)(1u << i);   // mark on attempt: fire-once + evalNow-safe
    }
}

void packDistributedConfig(const DistributedConfig& c, uint8_t out[DIST_CONFIG_BYTES]) {
    out[0] = c.enabled ? 1 : 0;
    out[1] = (uint8_t)(c.windowStartMin & 0xFF);       // u16 little-endian
    out[2] = (uint8_t)(c.windowStartMin >> 8);
    out[3] = (uint8_t)(c.windowEndMin & 0xFF);
    out[4] = (uint8_t)(c.windowEndMin >> 8);
    out[5] = (uint8_t)(c.perZoneMin & 0xFF);
    out[6] = (uint8_t)(c.perZoneMin >> 8);
    out[7] = c.fertCount;
    out[8] = 0;                                        // reserved
    out[9] = 0;
}

DistributedConfig unpackDistributedConfig(const uint8_t in[DIST_CONFIG_BYTES]) {
    DistributedConfig c;
    c.enabled        = in[0] != 0;
    c.windowStartMin = (uint16_t)(in[1] | ((uint16_t)in[2] << 8));
    c.windowEndMin   = (uint16_t)(in[3] | ((uint16_t)in[4] << 8));
    c.perZoneMin     = (uint16_t)(in[5] | ((uint16_t)in[6] << 8));
    c.fertCount      = in[7];
    return c;
}

void packScheduleEntry(const ScheduleEntry& e, uint8_t out[SCHED_ENTRY_BYTES]) {
    out[0] = e.id;
    out[1] = e.zoneIndex;
    out[2] = e.hour;
    out[3] = e.minute;
    out[4] = (uint8_t)(e.durationSec & 0xFF);          // u16 little-endian
    out[5] = (uint8_t)(e.durationSec >> 8);
    out[6] = e.daysMask;
    out[7] = (uint8_t)e.fertOverride;
    out[8] = e.enabled ? 1 : 0;
    out[9] = 0;                                        // reserved
}

ScheduleEntry unpackScheduleEntry(const uint8_t in[SCHED_ENTRY_BYTES]) {
    ScheduleEntry e;
    e.id           = in[0];
    e.zoneIndex    = in[1];
    e.hour         = in[2];
    e.minute       = in[3];
    e.durationSec  = (uint16_t)(in[4] | ((uint16_t)in[5] << 8));
    e.daysMask     = in[6];
    e.fertOverride = in[7] <= 2 ? (FertOverride)in[7] : FertOverride::Auto;
    e.enabled      = in[8] != 0;
    return e;
}

} // namespace tinkle
