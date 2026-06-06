#pragma once
#include <stdint.h>

// Clock — wall-clock time with NTP sync + free-running millis() fallback (firmware
// spec §13, DEC-009).
//
// Platform-independent (src/core): all SNTP / timezone / DST machinery lives behind an
// injected IWallClock seam (the ESP32 binding wraps configTzTime + the system RTC,
// src/esp32/system_clock.h; the host test runner drives a fake). The future DS3231 RTC
// drops in as just another IWallClock — the core free-run + HH:MM/weekday derivation
// never changes (§13 "leave a clean seam").
//
// SEAM CONTRACT: IWallClock hands core LOCAL epoch seconds — UTC already offset for the
// farm's timezone and DST. The ESP32 shim owns that offset (configTzTime + tm_gmtoff),
// keeping this core a pure epoch->fields derivation with no timezone database. Core's job:
//   1. anchor an authoritative reading to a millis() instant and free-run between reads,
//   2. track validity (synced at least once since boot) for the display's clockValid (§12),
//   3. derive local HH:MM + weekday for the display and the scheduler's per-minute eval (#27).
//
// FREE-RUN (§13): once synced, wall time = anchorEpoch + (millis() - anchorMs); drift is
// acceptable for irrigation. Re-anchored periodically, so a live NTP/RTC source self-
// corrects. A DST flip while the network is DOWN is not tracked until the next resync re-
// anchors (documented limitation, DEC-009 — sub-hour, irrigation-irrelevant). Before the
// first sync wall time is unknown: valid()==false and the display shows "--:--".

namespace tinkle {

// The wall-clock source. localEpoch() returns true and writes LOCAL epoch seconds when a
// sync source has valid time; false before first sync / when no source is available. It
// may keep returning true with a freely-advancing value once set — Clock re-anchors on a
// throttled cadence, so a live source corrects free-run drift.
struct IWallClock {
    virtual bool localEpoch(uint32_t& epochOut) = 0;
    virtual ~IWallClock() = default;
};

// Pack broken-down LOCAL calendar fields into local epoch seconds (proleptic Gregorian,
// 1970-01-01 = 0). The inverse of the epoch->fields math in wall(): an IWallClock shim
// that only has a struct-tm-style reading (localtime_r, which applies TZ + DST) feeds the
// core's local-epoch seam through this, so the civil arithmetic stays host-testable rather
// than buried in an ESP32-only header. month/day are 1-based; valid for years 1970..2105.
uint32_t epochFromCivil(int year, unsigned month, unsigned day,
                        unsigned hour, unsigned minute, unsigned second);

// Local broken-down time. Fields are meaningful only when Clock::valid(); gate on it.
struct WallTime {
    uint8_t hour;     // 0..23 local
    uint8_t minute;   // 0..59
    uint8_t second;   // 0..59
    uint8_t weekday;  // 0=Sunday .. 6=Saturday (matches tm_wday; scheduler maps daysMask, #27)
};

class Clock {
public:
    // How often localEpoch() is polled. Before lock-on we retry briskly so the clock
    // snaps valid soon after the network appears (WiFi join lands Phase 4); once synced we
    // only re-anchor hourly, so the free-run path is genuinely exercised between NTP reads.
    static constexpr uint32_t ATTEMPT_INTERVAL_MS = 1000;       // unsynced retry cadence
    static constexpr uint32_t RESYNC_INTERVAL_MS  = 3600000;    // 1 h drift correction

    explicit Clock(IWallClock& src);

    // First sync attempt; anchors immediately if the source is already valid. Call once at
    // boot after the source is configured.
    void begin(uint32_t nowMs);

    // Poll the source on the throttled cadence; re-anchor on a fresh authoritative reading.
    void tick(uint32_t nowMs);

    bool     valid() const { return synced_; }       // clockValid (§12)
    uint32_t epoch(uint32_t nowMs) const;             // local epoch sec, free-run; 0 if !valid
    WallTime wall(uint32_t nowMs)  const;             // zeroed if !valid — gate on valid()

    // True once per local-minute boundary — the scheduler's per-minute eval edge (§13, #27).
    // Fires on the first call after sync (eval at startup) and whenever the minute differs
    // from the last seen. Always false until valid(). Stateful: mutates the last-seen minute.
    //
    // CONTRACT (#27): an hourly NTP resync can step the epoch backward by a sub-second
    // drift correction, so this can fire an extra time around a minute boundary. We do NOT
    // clamp the clock forward — tracking truth matters more — so the scheduler must be
    // idempotent per (entry, minute): a re-eval of an already-started run must be a no-op.
    bool minuteRolled(uint32_t nowMs);

private:
    void resync(uint32_t nowMs);

    IWallClock& src_;
    bool        synced_        = false;
    uint32_t    epochAnchor_   = 0;     // local epoch sec at the anchor
    uint32_t    msAnchor_      = 0;     // millis() at the anchor
    uint32_t    lastAttemptMs_ = 0;     // throttle for resync polling
    uint32_t    lastMinute_    = 0;     // total local minutes at last minuteRolled()
    bool        minuteSeen_    = false; // false until the first minuteRolled() after sync
};

} // namespace tinkle
