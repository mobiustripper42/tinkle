#pragma once
#include <stdint.h>

// RunLog — the persisted run-history ring (firmware spec §4 step 7 / §8 / §15, DEC-018).
//
// Platform-independent and DEPENDENCY-FREE on purpose: it includes nothing from the rest
// of src/core (no RunController, no Fault enum) so it compiles + host-tests as a pure data
// structure, and so RunController can own one by value without a circular include. The run
// outcome is its own RunResult enum and the fault is carried as a raw code byte; the
// translation to/from RunController's Fault enum happens at the push/read sites.
//
// Shape (DEC-018): a fixed ring of RUNLOG_DEPTH entries. RunController pushes one at SETTLE
// (and on a fault); the ring HEAD is the single "last run" the status API exposes — one
// source of truth. Persisted as one packed `runlog` NVS blob mirroring the `sched` blob
// (Persistence::saveRunLog / loadRunLog), additive under DEC-008 (no schema_ver bump).

namespace tinkle {

// Run outcome. Mirrors the old RunController::RunSummary::Result (which this retires) — the
// single run-result enum now lives here, packed into 2 bits of the entry flags byte.
enum class RunResult : uint8_t { None, Completed, Stopped, Faulted };

// One past run, unpacked. centigallons (hundredths of a gallon) keeps volume integer +
// NVS-blob-portable (DEC-018) — no float in the record. startEpoch is meaningful for
// wall-clock display only when clockWasValid; otherwise the SPA renders relative-to-uptime.
struct RunEntry {
    uint32_t  startEpoch    = 0;     // local epoch sec at run start (0 / implausible => !valid)
    uint8_t   zoneIndex     = 0;
    uint16_t  durationSec   = 0;
    uint16_t  centigallons  = 0;     // 0..65535 = 0..655.35 gal
    bool      fertigate     = false;
    RunResult result        = RunResult::None;
    bool      clockWasValid = false; // NTP had synced when the run started (per-entry, DEC-018)
    uint8_t   faultCode     = 0;     // (uint8_t)Fault; 0 = Fault::None
};

// §15: bumpable to 64 later (write-wear, not partition space, is the ceiling — DEC-018).
constexpr uint8_t  RUNLOG_DEPTH       = 32;
constexpr uint16_t RUNLOG_ENTRY_BYTES = 11;
constexpr uint16_t RUNLOG_BLOB_BYTES  = RUNLOG_DEPTH * RUNLOG_ENTRY_BYTES;  // 352

// 2025-01-01 00:00:00 (epoch sec). A run start below this is treated as pre-NTP free-run and
// logged with clockWasValid CLEAR — never as a 1970-ish wall-clock (DEC-018 epoch-sanity).
constexpr uint32_t RUNLOG_MIN_VALID_EPOCH = 1735689600u;

// Packed 11-byte record (DEC-018), little-endian, mirroring packScheduleEntry:
//   startEpoch u32 | zoneIndex u8 | durationSec u16 | centigallons u16 | flags u8 | faultCode u8
// flags: bit0 fertigate | bits1-2 result | bit3 clockWasValid.
void     packRunEntry(const RunEntry& e, uint8_t out[RUNLOG_ENTRY_BYTES]);
RunEntry unpackRunEntry(const uint8_t in[RUNLOG_ENTRY_BYTES]);

// Fixed-depth ring. push() overwrites the oldest entry once full; head() is the most-recent
// run (the "last run"). Stores entries unpacked in RAM; packing crosses the NVS boundary only.
class RunLog {
public:
    // Append a run. Newest entry becomes the head.
    void push(const RunEntry& e);

    uint8_t count() const { return count_; }                 // 0..RUNLOG_DEPTH
    bool    empty() const { return count_ == 0; }

    // The most-recent entry. Returns a default (result=None) RunEntry when the ring is empty,
    // so /api/status lastRun renders "none" before the first run.
    const RunEntry& head() const;

    // Newest-first indexing: at(0) == head(), at(count-1) == oldest. Out-of-range -> default.
    RunEntry at(uint8_t i) const;

    // NVS blob: pack the live entries OLDEST-first into out (so deserialize replays them in
    // order and the head lands newest). Returns bytes written (count * RUNLOG_ENTRY_BYTES,
    // never more than cap). A cap below one full ring truncates to whole entries.
    uint16_t serialize(uint8_t* out, uint16_t cap) const;

    // Rehydrate from a blob. Tolerant of a torn/short blob (truncates to whole entries) and of
    // an over-long one (keeps the last RUNLOG_DEPTH). Empty/absent blob -> empty ring.
    void deserialize(const uint8_t* in, uint16_t len);

private:
    RunEntry ring_[RUNLOG_DEPTH] = {};
    uint8_t  headIdx_ = 0;     // index of the most-recent entry (valid only when count_ > 0)
    uint8_t  count_   = 0;     // entries stored, capped at RUNLOG_DEPTH
};

} // namespace tinkle
