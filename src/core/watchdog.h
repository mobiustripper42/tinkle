#pragma once
#include <stdint.h>
#include "run_controller.h"   // RunState + Fault — the shared vocabulary
#include "valve_driver.h"     // IGpio

// Watchdog — the ESP32 half of the §9 handshake (#5.2). Emits the heartbeat the
// ATtiny85 (WatchdogTrip) listens for and turns the trip line into a Fault verdict.
// Platform-independent (src/core): pin writes go through the injected IGpio, the
// trip-line LEVEL is read by the caller and passed in (IGpio stays output-only),
// and time is an explicit millis() value — host-tested like everything else.
//
// Heartbeat encoding (must mirror watchdog_trip.h): toggle HEARTBEAT_OUT every
// HEARTBEAT_MS, but ONLY while the pump is commanded — START_PUMP and RUNNING.
// Entering the window toggles immediately (the ATtiny arms on the first edge,
// well inside the zone valve's travel headroom); leaving it parks the line LOW.
// Toggling from the healthy loop path is the point: if the loop stalls, the
// toggling stops, and that absence IS the signal (§9).
//
// Trip verdict (mirrors FlowFaultDetector): tick() RETURNS the fault to raise and
// never actuates — the caller routes a non-None verdict to RunController (which,
// per DEC-023, ABORTS the current run and preserves the queue — Watchdog is
// non-latching; the relay already cut the pump, so the ESP32's job is bookkeeping
// and unwinding, not blocking the rest of the schedule). Trip asserted during
// any ACTIVE state (anything but Idle/Fault) is the verdict: water is staged
// or moving and the hardware backstop says stop. Trip asserted while IDLE is not
// a fault — per the encoding it can only be a not-yet-released lockout, the §4
// pre-open gate (setWatchdogTripped) already holds new runs, and the line
// self-releases once the heartbeat stays quiet.
//
// TRIP QUALIFICATION (DEC-023, the 2026-07-09 field false positive): the trip
// line (GPIO36, 10k pull-up, ATtiny open-drain) is sampled once per tick, and a
// single glitched read — pump-relay switching transients land exactly in the
// run-end tail — used to latch a fault and kill the day's schedule. A raw
// assertion now counts only after it has held CONTINUOUSLY for tripConfirmMs.
// A genuine lockout holds the line low for >= HB_TIMEOUT_MS (2 s) by design, so
// qualification delays a real verdict by 100 ms and erases sub-100 ms noise.

namespace tinkle {

class Watchdog {
public:
    struct Config {
        uint32_t heartbeatMs   = 250;   // §15 HEARTBEAT_MS — toggle half-period
        uint32_t tripConfirmMs = 100;   // §15 TRIP_CONFIRM_MS — continuous assertion
                                        // required before a trip read counts (DEC-023)
    };

    Watchdog(IGpio& gpio, uint8_t heartbeatPin, const Config& cfg)
        : g_(gpio), pin_(heartbeatPin), cfg_(cfg) {}

    // Configure the heartbeat pin and park it LOW (no run, no beat). Call at boot.
    void begin(uint32_t nowMs);

    // Call every loop tick with the post-tick run state and the trip-line level
    // (already polarity-mapped by the caller: true = ATtiny asserting tripped).
    // Returns the fault to raise this tick, or Fault::None.
    Fault tick(RunState state, bool tripAsserted, uint32_t nowMs);

    // The QUALIFIED trip level after tick() — raw assertion held >= tripConfirmMs.
    // This, not the raw pin read, feeds RunController::setWatchdogTripped (the §4
    // pre-open gate), so a glitch can't hold a queued run either.
    bool tripConfirmed() const { return tripConfirmed_; }

    // Introspection for tests / status.
    bool emitting()       const { return emitting_; }
    bool heartbeatLevel() const { return level_; }

private:
    IGpio&   g_;
    uint8_t  pin_;
    Config   cfg_;
    bool     emitting_       = false;
    bool     level_          = false;
    uint32_t lastToggleMs_   = 0;
    bool     tripSeen_       = false;   // raw assertion present last tick
    uint32_t tripStartMs_    = 0;       // when the current continuous assertion began
    bool     tripConfirmed_  = false;
};

} // namespace tinkle
