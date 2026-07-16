#pragma once
#include <stdint.h>
#include "run_controller.h"   // IRunSink, RunRequest
#include "clock.h"            // Clock, WallTime
#include "valve_driver.h"     // ValveConfig::MAX_ZONES — default zone count

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

// Fixed-width pack format for the §8 NVS schedule blob (Phase 4 save-on-edit). One
// entry = 10 bytes, explicit little-endian for the u16 — the blob must survive a
// firmware rebuild, so no struct-memcpy (padding/ABI would silently version it).
// Byte 9 is reserved (0) for forward-compat without a schema bump.
constexpr uint16_t SCHED_ENTRY_BYTES = 10;
void packScheduleEntry(const ScheduleEntry& e, uint8_t out[SCHED_ENTRY_BYTES]);
ScheduleEntry unpackScheduleEntry(const uint8_t in[SCHED_ENTRY_BYTES]);

// --- Distributed Watering (DEC-024) ---------------------------------------------------
// A mutually-exclusive alternative to the entry schedule. Even-spread the day's water:
// derive N cycles across [windowStartMin, windowEndMin]; each cycle fires all live zones
// back-to-back (queue depth = zoneCount, under MAX_QUEUE), so each bed soaks between its
// own runs while the others water — the rotation is the soak, no RunController state.
// One packed record under its own NVS key (additive, DEC-008; the entry schedule is
// untouched). The plan math here is the single source of truth the SPA preview mirrors.
struct DistributedConfig {
    bool     enabled        = false;   // mode flag: true => runs INSTEAD of the entry schedule
    uint16_t windowStartMin = 0;       // minutes from local midnight, 0..1439
    uint16_t windowEndMin   = 0;       // must be > windowStartMin
    uint16_t perZoneMin     = 0;       // total watering minutes per zone per day
    uint8_t  fertCount      = 0;       // fertigate the first N cycles (whole-cycle; 0..DIST_MAX_RUNS)
};

// Firmware envelope (DEC-024): a run floor and a per-day cap. Runs never go below the
// floor; cycles never exceed the cap. These bound the derived plan below.
constexpr uint16_t DIST_RUN_FLOOR_MIN    = 7;    // hard minimum run length (agronomic, DEC-024)
constexpr uint8_t  DIST_MAX_RUNS         = 6;    // max cycles/runs per zone per day
constexpr uint16_t DIST_RUN_OVERHEAD_SEC = 25;   // per-run valve/pump/settle overhead estimate
                                                 // for the fit check (pending #98 travel confirm)

// The derived plan: what the controller will actually do for a given config. `valid` is
// false when the config can't be scheduled (window too short, below the floor, or the
// cycles don't fit) — the Scheduler then emits nothing, and the SPA blocks the save.
struct DistributedPlan {
    bool     valid        = false;
    uint8_t  cycles       = 0;                    // runs per zone = number of cycle boundaries
    uint16_t runLenSec    = 0;                    // per-run duration
    uint16_t cycleStartMin[DIST_MAX_RUNS] = {};   // minute-of-day each cycle fires (bookended)
};

// Pure function (host-tested): derive the plan from a config + the live zone count. Shared
// by the Scheduler (to emit) and the API/SPA (to preview + validate) so they never diverge.
DistributedPlan computeDistributedPlan(const DistributedConfig& cfg, uint8_t zoneCount);

// Packed fixed-width record for the `dist` NVS key (DEC-008 additive; own key, no schema
// bump). 10 bytes: enabled, windowStartMin(LE u16), windowEndMin(LE u16), perZoneMin(LE
// u16), fertCount, reserved. Explicit LE like the schedule blob — survives a rebuild.
constexpr uint16_t DIST_CONFIG_BYTES = 10;
void packDistributedConfig(const DistributedConfig& c, uint8_t out[DIST_CONFIG_BYTES]);
DistributedConfig unpackDistributedConfig(const uint8_t in[DIST_CONFIG_BYTES]);

class Scheduler {
public:
    static constexpr uint8_t MAX_ENTRIES = 16;   // V1 cap; the UI/NVS bound it later

    Scheduler(IRunSink& sink, Clock& clock, uint8_t zoneCount = ValveConfig::MAX_ZONES);

    // --- schedule editing (the web API / persistence drive these, Phase 4) ---
    bool                 add(const ScheduleEntry& e);     // false if full
    void                 clear();
    uint8_t              count() const { return count_; }
    const ScheduleEntry& entry(uint8_t i) const { return entries_[i < count_ ? i : 0]; }

    // --- Distributed Watering (DEC-024) — mutually exclusive with the entry schedule ---
    void                     setDistributed(const DistributedConfig& c) { dist_ = c; }
    const DistributedConfig& distributed() const { return dist_; }

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
    // Distributed-mode evaluation (DEC-024): fire whichever cycle(s) start this minute,
    // once per day-cycle. Keyed on cycle index (not the wall minute), so an evalNow()
    // re-entry within the same minute cannot double-emit a cycle.
    void evaluateDistributed(uint8_t hour, uint8_t minute, uint32_t dayOrdinal, uint32_t nowMs);
    // Resolve the fertigate flag for a due entry, consuming the daily auto-fert slot only
    // when the run actually enqueues (caller passes the slot decision back on success).
    bool resolveFert(const ScheduleEntry& e, uint32_t dayOrdinal, bool& consumesAutoSlot) const;

    IRunSink& sink_;
    Clock&    clock_;
    uint8_t   zoneCount_;           // live zones per cycle (<= ValveConfig::MAX_ZONES)

    ScheduleEntry entries_[MAX_ENTRIES];
    uint8_t       count_ = 0;

    DistributedConfig dist_;        // DEC-024; dist_.enabled routes evaluate() here instead

    bool     evaledOnce_   = false;
    uint32_t lastEvalMin_  = 0;     // absolute local minute last evaluated (idempotency key)

    // Fert policy (§6): the first auto run of each calendar day fertigates.
    bool     autoFertUsed_ = false;
    uint32_t autoFertDay_  = 0;     // local day ordinal that consumed the auto-fert slot

    // Distributed idempotency: which cycles have fired today (bit i = cycle i). Reset when
    // the day ordinal changes. Immune to evalNow() re-entry — a fired cycle never re-emits.
    uint32_t distFiredDay_  = 0xFFFFFFFFu;   // sentinel: no day fired yet
    uint8_t  distFiredMask_ = 0;

    uint32_t dropped_ = 0;
};

} // namespace tinkle
