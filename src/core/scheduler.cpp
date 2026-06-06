#include "scheduler.h"

namespace tinkle {

Scheduler::Scheduler(IRunSink& sink, Clock& clock) : sink_(sink), clock_(clock) {}

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

} // namespace tinkle
