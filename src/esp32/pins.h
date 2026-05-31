#pragma once
#include <stdint.h>

// Tinkle ESP32 GPIO map. Mirrors docs/tinkle_wiring.html (§B) — the wiring doc
// is the source of truth for pins; if this disagrees with it, the wiring doc wins.
//
// ESP32 DevKitC 38-pin. Avoids flash pins (6-11), TX/RX (1/3), and boot-sensitive
// strapping-pin use. GPIO 34/35/36/39 are input-only (no internal pull-ups) —
// buttons and the watchdog-trip line use external 10k pull-ups + 100nF caps.

// H-bridges (DRV8871 IN1/IN2). Open pulse = IN1 high, close pulse = IN2 high.
// INVARIANT: never both inputs HIGH on the same bridge.
constexpr uint8_t Z1_IN1   = 13;
constexpr uint8_t Z1_IN2   = 14;
constexpr uint8_t Z2_IN1   = 16;
constexpr uint8_t Z2_IN2   = 17;
constexpr uint8_t DIV_IN1  = 18;   // diverter THROUGH (fertigate)
constexpr uint8_t DIV_IN2  = 19;   // diverter AROUND (plain). ~6s travel, not a 75ms latch.

// Master & pump.
constexpr uint8_t MASTER_FET  = 21;  // IRLZ44N gate (logic-level). Flyback diode on coil.
constexpr uint8_t PUMP_RELAY  = 22;  // relay trigger. Snubber/flyback on coil.

// Sensing.
constexpr uint8_t FLOW_PIN    = 27;  // hall pulse, interrupt. Level-shift 5V->3.3V upstream.

// Display (TM1637).
constexpr uint8_t TM_CLK = 25;
constexpr uint8_t TM_DIO = 26;

// Buttons — momentary to GND, input-only pins, external pull-up + debounce cap.
constexpr uint8_t BTN1 = 34;  // Zone 1 run
constexpr uint8_t BTN2 = 35;  // Zone 2 run
constexpr uint8_t BTN3 = 39;  // Stop / cancel all (long-press = fault clear)

// Button LED rings — 12V, low-side via ULN2803 (active-high to ULN input).
constexpr uint8_t LED1 = 32;
constexpr uint8_t LED2 = 33;
constexpr uint8_t LED3 = 23;

// Watchdog handshake (ATtiny85).
// HEARTBEAT_OUT carries a square wave emitted ONLY while a run is active
// (DEC-004): "heartbeat present" == "a run is in progress". No separate
// run-active line exists — the wiring doc allocates none.
constexpr uint8_t HEARTBEAT_OUT = 4;
constexpr uint8_t WD_TRIPPED_IN = 36;  // ATtiny "tripped" -> ESP32 (input-only). For logging/display.

// Zones modeled as a table so the count is data-driven (firmware spec §3).
struct Zone { uint8_t in1, in2, ledPin, btnPin; const char* name; };
constexpr Zone ZONES[] = {
    { Z1_IN1, Z1_IN2, LED1, BTN1, "Zone 1" },
    { Z2_IN1, Z2_IN2, LED2, BTN2, "Zone 2" },
};
constexpr uint8_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
