#pragma once
#include <stdint.h>

// NightlyReboot — decides WHEN to self-reboot for field reliability. A nightly power-cycle
// clears the WiFi-stack / heap stalls that accumulate over long uptime (the intermittent
// "SPA loads then disconnects" field mode). Platform-independent + host-tested: the ESP32
// layer supplies the clock/idle inputs and performs the actual ESP.restart().
//
// Policy (safety first — never interrupt watering):
//   - Once per local calendar day, inside a short post-midnight window.
//   - ONLY when the controller is safely idle: no run, empty queue, not faulted, no OTA in
//     flight. If busy at 00:00, it reboots at the first idle tick still inside the window;
//     if it stays busy the whole window, it skips that night (fail-quiet — the same stance
//     as the mid-run power-cut we refuse to cause: a reboot drops the RAM run queue).
//   - A minimum-uptime guard prevents a reboot loop: the device that just rebooted at 00:00
//     comes back inside the window but won't reboot again until well past it.
//
// The reboot is deliberately safe by construction: idle => the heartbeat is already parked
// and the ATtiny relay de-armed, so the reboot just extends the pump-unpowered state into
// the next boot (same reasoning as the DEC-022 OTA reboot).

namespace tinkle {

class NightlyReboot {
public:
    struct Config {
        uint16_t windowLenMin = 60;                   // post-midnight window [00:00, +len) to catch idle
        uint32_t minUptimeMs  = 2u * 3600u * 1000u;   // loop-guard: don't reboot within 2 h of boot
    };

    // Two ctors instead of a `= Config{}` default arg: brace-initializing a struct that has
    // default member initializers is ill-formed at -std=gnu++11 (arduino-esp32 2.0.x).
    NightlyReboot() {}
    explicit NightlyReboot(const Config& cfg) : cfg_(cfg) {}

    // Returns true EXACTLY once on the tick a reboot should happen; the caller then flushes
    // pending state and restarts. `minOfDay` is the LOCAL minute-of-day (0..1439), `dayOrdinal`
    // the local calendar-day ordinal (both as the Scheduler derives them). `safeToReboot` is
    // the caller-composed idle predicate (idle && queue empty && !faulted && !otaActive).
    bool due(uint32_t dayOrdinal, uint16_t minOfDay, uint32_t uptimeMs,
             bool clockValid, bool safeToReboot) {
        if (!clockValid)                    return false;   // no wall clock -> no midnight to anchor
        if (uptimeMs < cfg_.minUptimeMs)    return false;   // just booted -> don't re-reboot (loop guard)
        if (minOfDay >= cfg_.windowLenMin)  return false;   // outside the post-midnight window
        if (dayOrdinal == lastRebootDay_)   return false;   // already rebooted today
        if (!safeToReboot)                  return false;   // busy now -> retry a later tick in-window
        lastRebootDay_ = dayOrdinal;
        return true;
    }

private:
    Config   cfg_;
    uint32_t lastRebootDay_ = 0xFFFFFFFFu;   // sentinel: nothing rebooted yet
};

} // namespace tinkle
