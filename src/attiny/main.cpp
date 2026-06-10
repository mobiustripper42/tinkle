#include <Arduino.h>

// Tinkle — ATtiny85 safety watchdog. Independent fail-dry backstop.
//
// It arms the SAFETY RELAY (normally-open, energize-to-pass) that feeds the armed
// 24V to the PUMP — the water source (there is no master valve, DEC-012). The
// ATtiny never commands anything — it can only gate the pump's power. Drop ARM and
// the pump dies → no source → no water, whatever the valves do. This is the
// hardware stand-in for the human who used to watch the hose.
//
// ENCODING (DEC-004 / firmware spec §9):
//   The ESP32 emits a heartbeat square wave ONLY while a watering run is active.
//   So "heartbeat present" == "a run is in progress" — there is NO separate
//   run-active line (the wiring doc allocates none).
//
//   Hold ARM asserted only while BOTH:
//     (a) a heartbeat edge has been seen within HB_TIMEOUT_MS (2000), AND
//     (b) continuous armed time has not exceeded HARD_MAX_RUNTIME (30 min),
//         timed on the ATtiny's OWN clock — never trusting the ESP32's sense
//         of time.
//   Either fails -> de-assert ARM (relay opens) AND assert TRIPPED to the ESP32.
//
//   Power-up / reset default: ARM de-asserted (fail-dry on watchdog reboot).
//
// SCAFFOLD ONLY. Pin assignments and the edge-timer logic get filled in Phase 5
// once the bench wiring exists. Keep this sketch dependency-free and tiny.

// constexpr unsigned long HB_TIMEOUT_MS    = 2000;
// constexpr unsigned long HARD_MAX_RUNTIME = 30UL * 60UL * 1000UL;
// Pins TBD on bench: HEARTBEAT_IN, ARM_OUT, TRIPPED_OUT.

void setup() {
    // De-assert ARM first thing — fail-dry on every reset.
}

void loop() {
    // Edge-detect heartbeat; run the two trip conditions; drive ARM / TRIPPED.
}
