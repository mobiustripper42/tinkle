#pragma once
#include <stdint.h>
#include "run_controller.h"

// FaultManager — the §14 fault surface on top of RunController's latch (#5.3
// software half). Three jobs:
//
//   1. LOG: a fixed ring of recent fault entries (code + millis timestamp) for
//      the §10 status API / SPA Faults screen and the serial log. RunController
//      stays lean; this module edge-detects its latched fault each tick.
//   2. RESOLVED-CONDITION CLEAR GATE: §14 recovery requires an explicit clear
//      AND the underlying condition resolved. Detectors push their condition's
//      CURRENT truth in via setConditionActive() (e.g. the watchdog trip line
//      still asserted, flow still pulsing while idle); requestClear() refuses
//      while the active fault's condition holds. This closes the DEC-006 gate
//      noted in main.cpp — a long-press on a latched-but-unresolved fault is now
//      a visible no-op instead of a false clear.
//   3. Mediates the clear: every clear path (button long-press now, the Phase 4
//      /api/fault/clear endpoint later) goes through requestClear(), so the gate
//      can't be bypassed.
//
// Codes nobody pushes a condition for (e.g. CalRange — a one-shot rejection)
// default to "not active" and clear freely. Platform-independent (src/core),
// host-tested with injected faults. Timestamps are millis()-domain; the Phase 4
// status layer can join them to wall time if wanted.

namespace tinkle {

class FaultManager {
public:
    // ~16 entries, persisted (#90, the DEC-018 follow-up — was 8 and RAM-only). The §8 fault
    // log now survives reboot, mirroring the `runlog` blob.
    static constexpr uint8_t LOG_SIZE = 16;

    struct Entry {
        Fault    code          = Fault::None;
        uint32_t epoch         = 0;       // local epoch sec at log time (0 / implausible => !valid)
        bool     clockWasValid = false;   // NTP had synced + epoch plausible when logged (#90)
    };

    // Packed NVS record (6 B): epoch u32 LE | code u8 | flags u8 (bit0 clockWasValid).
    static constexpr uint16_t ENTRY_BYTES = 6;
    static constexpr uint16_t BLOB_BYTES  = LOG_SIZE * ENTRY_BYTES;   // 96

    explicit FaultManager(RunController& rc) : rc_(rc) {}

    // Call every loop tick: edge-detects a newly latched fault into the log. epoch + clockValid
    // are pushed in from main (the runlog idiom — this module has no clock); the pre-2025
    // epoch-sanity guard runs here so a pre-NTP fault logs clockWasValid CLEAR (#90).
    void tick(uint32_t epoch, bool clockValid);

    // Append a NON-LATCHING entry to the log ring — maintenance findings (the
    // DEC-014 valve-rest flag) that deserve the §10 Faults surface without
    // commanding the safe state or blocking runs. None is ignored.
    void note(Fault code, uint32_t epoch, bool clockValid);

    // Detectors push the current truth of their fault's underlying condition.
    // Out-of-range codes are ignored.
    void setConditionActive(Fault code, bool active);
    bool conditionActive(Fault code) const;

    // The §14 gate. True iff a fault is latched and its condition has resolved.
    bool clearAllowed() const;

    // Clear the latched fault iff allowed. Returns true on an actual clear —
    // callers key the success ack (§12) off this, so a blocked clear stays a
    // visible no-op.
    bool requestClear();

    // Log introspection, newest first (i = 0 is the most recent entry).
    uint8_t logCount() const { return logCount_; }
    Entry   logEntry(uint8_t i) const;

    // NVS persistence (#90), mirroring RunLog: pack the ring OLDEST-first; deserialize replays
    // in order so the head lands newest. dirty() drives main's debounced write; rehydrate does
    // not dirty.
    uint16_t serialize(uint8_t* out, uint16_t cap) const;
    void     deserialize(const uint8_t* in, uint16_t len);
    bool     dirty() const { return dirty_; }
    void     markPersisted()   { dirty_ = false; }

private:
    static constexpr uint8_t CODE_COUNT = (uint8_t)Fault::Count;   // sized off the enum
    static uint8_t idx(Fault code) { return (uint8_t)code; }
    void append(Fault code, uint32_t epoch, bool clockWasValid);

    RunController& rc_;
    Fault          prevFault_ = Fault::None;
    bool           condActive_[CODE_COUNT] = {};

    Entry   log_[LOG_SIZE] = {};
    uint8_t logHead_  = 0;   // next write slot
    uint8_t logCount_ = 0;   // valid entries, capped at LOG_SIZE
    bool    dirty_    = false;   // a fault was logged, awaiting the debounced NVS write
};

} // namespace tinkle
