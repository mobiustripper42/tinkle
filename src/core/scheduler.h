#pragma once
#include <stdint.h>
#include "run_controller.h"   // IRunSink, RunRequest
#include "clock.h"            // Clock, WallTime

// Scheduler — evaluates schedule entries once per local minute, enqueues due runs through
// the RunController (via the narrow IRunSink seam), and applies the fertigation policy
// (firmware spec §13 + §6, DEC-009 eval contract).
//
// Platform-independent (src/core): driven by an injected Clock and IRunSink, host-tested
// with a fake clock and a fake run sink. Overlap is not the Scheduler's to solve — the
// RunController already queues sequential runs and rejects when full (§4); a due run that
// can't enqueue is dropped and counted (§13 "drop + log if exceeded").
//
// IDEMPOTENCY (DEC-009): evaluation is keyed on the absolute local minute and runs at most
// once per minute, so the hourly-NTP-resync backward nudge that can re-fire the minute edge
// never double-enqueues a due run. "Eval on edit" (§13) is exposed as evalNow() for when the
// schedule changes between minute boundaries.
//
// PERSISTENCE: schedule entries are NOT persisted yet. There is no editor until the web
// config API (Phase 4), so there is nothing to store; that API will own save-on-edit and
// drive add()/clear() here, then mirror the entries to NVS through the Persistence store's
// own keys (DEC-008). The §13 entry model and the engine land now; the NVS keys land with
// the thing that edits them.

namespace tinkle {

// Per-entry fertigation override (§6). Auto = scheduler policy (first enabled run of the
// calendar day fertigates); On / Off force the diverter state regardless of policy.
enum class FertOverride : uint8_t { Auto = 0, On = 1, Off = 2 };

// A scheduled run (§13). daysMask: bit w set => runs on weekday w, matching Clock's
// weekday numbering (0 = Sunday .. 6 = Saturday); 0x7F = daily.
struct ScheduleEntry {
    uint8_t      id           = 0;
    uint8_t      zoneIndex    = 0;
    uint8_t      hour         = 0;    // 0..23 local
    uint8_t      minute       = 0;    // 0..59
    uint16_t     durationSec  = 0;
    uint8_t      daysMask     = 0;    // bit-per-weekday; 0x7F = daily
    FertOverride fertOverride = FertOverride::Auto;
    bool         enabled      = false;
};

class Scheduler {
public:
    static constexpr uint8_t MAX_ENTRIES = 16;   // V1 cap; the UI/NVS bound it later

    Scheduler(IRunSink& sink, Clock& clock);

    // --- schedule editing (the web API / persistence drive these, Phase 4) ---
    bool                 add(const ScheduleEntry& e);     // false if full
    void                 clear();
    uint8_t              count() const { return count_; }
    const ScheduleEntry& entry(uint8_t i) const { return entries_[i < count_ ? i : 0]; }

    // Call every loop tick. Evaluates the schedule on each new local minute (no-op until
    // the clock is valid, and at most once per minute — see IDEMPOTENCY above).
    void tick(uint32_t nowMs);

    // Force an evaluation of the current minute now (§13 "checks ... on edit"). Re-arms the
    // minute guard so the edited schedule is seen immediately rather than at the next tick-over.
    void evalNow(uint32_t nowMs);

    // Runs dropped because the RunController refused them (full queue / fault), since boot.
    uint32_t dropped() const { return dropped_; }

private:
    void evaluate(uint32_t nowMs);
    // Resolve the fertigate flag for a due entry, consuming the daily auto-fert slot only
    // when the run actually enqueues (caller passes the slot decision back on success).
    bool resolveFert(const ScheduleEntry& e, uint32_t dayOrdinal, bool& consumesAutoSlot) const;

    IRunSink& sink_;
    Clock&    clock_;

    ScheduleEntry entries_[MAX_ENTRIES];
    uint8_t       count_ = 0;

    bool     evaledOnce_   = false;
    uint32_t lastEvalMin_  = 0;     // absolute local minute last evaluated (idempotency key)

    // Fert policy (§6): the first auto run of each calendar day fertigates.
    bool     autoFertUsed_ = false;
    uint32_t autoFertDay_  = 0;     // local day ordinal that consumed the auto-fert slot

    uint32_t dropped_ = 0;
};

} // namespace tinkle
