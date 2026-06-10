#include <Arduino.h>
#include "../core/watchdog_trip.h"

// Tinkle — ATtiny85 safety watchdog. Independent fail-dry backstop (#5.1).
//
// It arms the SAFETY RELAY (normally-open, energize-to-pass) that feeds the armed
// 24V to the PUMP — the water source (there is no master valve, DEC-012). The
// ATtiny never commands anything — it can only gate the pump's power. Drop ARM and
// the pump dies → no source → no water, whatever the valves do. This is the
// hardware stand-in for the human who used to watch the hose.
//
// All decision logic lives in ../core/watchdog_trip.h, compiled identically into
// the native test runner (DEC-016) — this sketch is just pins + polling. The
// ENCODING is documented there: heartbeat present == pump commanded; ARM held only
// while edges arrive within HB_TIMEOUT_MS and armed time < HARD_MAX_RUNTIME (timed
// on THIS chip's clock, never the ESP32's); TRIPPED asserts only on the
// HARD_MAX_RUNTIME lockout and self-releases after the heartbeat goes quiet.
//
// PINS (this header is the §9 source of truth for the ATtiny side; the wiring doc
// assigns only the ESP32 half — GPIO4 heartbeat out, GPIO36 trip in):
//   PB2 (phys 7)  HEARTBEAT_IN  <- ESP32 GPIO4. 3.3 V swing: at Vcc = 5 V that is
//                 marginal against the 0.6*Vcc AVR V_IH — run the ATtiny at 3.3 V
//                 (8 MHz internal is in spec) or level-shift. Bench-confirm.
//   PB1 (phys 6)  ARM_OUT       -> safety-relay driver, active HIGH. The driver
//                 base/gate needs an external pulldown so the relay stays open
//                 while this pin is Hi-Z during reset (fail dry through reboot).
//   PB0 (phys 5)  TRIPPED_OUT   -> ESP32 GPIO36, ACTIVE LOW, open-drain emulated:
//                 released = pin as INPUT (Hi-Z; the ESP32-side 10k pull-up idles
//                 the line HIGH), asserted = OUTPUT driving LOW. Never drives
//                 HIGH, so a 5 V ATtiny can't overvolt the 3.3 V input — and an
//                 absent/unpowered ATtiny reads "not tripped" (the line is
//                 informational; the relay is the safety).

namespace {

constexpr uint8_t HEARTBEAT_IN = PB2;
constexpr uint8_t ARM_OUT      = PB1;
constexpr uint8_t TRIPPED_OUT  = PB0;

const tinkle::WatchdogTrip::Config cfg;   // §15 defaults: 2 s timeout, 30 min ceiling
tinkle::WatchdogTrip trip(cfg);

void writeTripped(bool asserted) {
    if (asserted) {
        digitalWrite(TRIPPED_OUT, LOW);   // level before mode: no HIGH glitch
        pinMode(TRIPPED_OUT, OUTPUT);
    } else {
        pinMode(TRIPPED_OUT, INPUT);      // Hi-Z; ESP32-side pull-up = released
    }
}

} // namespace

void setup() {
    // De-assert ARM first thing — fail-dry on every reset.
    digitalWrite(ARM_OUT, LOW);
    pinMode(ARM_OUT, OUTPUT);
    writeTripped(false);
    pinMode(HEARTBEAT_IN, INPUT);
    trip.begin(digitalRead(HEARTBEAT_IN) == HIGH, millis());
}

void loop() {
    trip.tick(digitalRead(HEARTBEAT_IN) == HIGH, millis());
    digitalWrite(ARM_OUT, trip.armed() ? HIGH : LOW);
    writeTripped(trip.tripped());
}
