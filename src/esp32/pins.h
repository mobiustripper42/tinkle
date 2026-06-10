#pragma once
#include <stdint.h>

// Tinkle ESP32 GPIO map. Mirrors docs/tinkle_wiring.html (§B) — the wiring doc is
// the source of truth for pins; if this disagrees with it, the wiring doc wins.
//
// ESP32 DevKitC 38-pin. Avoids flash pins (6-11) and TX/RX (1/3). v1.4 (DEC-011):
// one low-side FET per valve (1 GPIO each), no H-bridges — so no strapping pins are
// needed and the DEC-007 GPIO12/15 brick-boot contortion is retired. GPIO 34/35/36/39
// are input-only (no internal pull-ups) — buttons and the watchdog-trip line use
// external 10k pull-ups + 100nF caps.

// Valve FETs — low-side, one IRLZ44N per valve. Energize (HIGH) = actuate; a series
// gate resistor + gate-to-GND pulldown holds each FET off (valve at rest) through
// ESP32 boot. The valve travels ~6-10 s and self-cuts at its limit, then holds open
// while energized; de-energize and a capacitor auto-returns it to rest.
constexpr uint8_t Z1_FET = 13;   // Zone 1 (NC): high = open
constexpr uint8_t Z2_FET = 14;   // Zone 2 (NC): high = open
constexpr uint8_t Z3_FET = 16;   // Zone 3 (NC): general-purpose hose outlet (build-for-three)

// Diverter — two 2-way legs driven together (DEC-013), replacing the v1.3 3-way.
constexpr uint8_t DIV_CLEAN_FET = 17;  // NO bypass leg:  low (rest) = open  = plain water
constexpr uint8_t DIV_FERT_FET  = 18;  // NC Dosatron leg: high       = open  = fertigate

// Pump — on the ARMED 24V (the fail-dry source gate, DEC-012). There is no master.
constexpr uint8_t PUMP_RELAY  = 22;  // relay trigger. Snubber/flyback on coil.

// Sensing.
constexpr uint8_t FLOW_PIN    = 27;  // hall pulse, interrupt. Level-shift 5V->3.3V upstream.

// Display (TM1637).
constexpr uint8_t TM_CLK = 25;
constexpr uint8_t TM_DIO = 26;

// Buttons — momentary to GND, input-only pins, external pull-up + debounce cap.
// One button per zone (DEC-006): idle press starts that zone; a press while any zone
// runs stops it; a >=3 s long-press of any button clears a latched fault. No dedicated
// stop button — "any press stops" makes one unnecessary.
constexpr uint8_t BTN1 = 34;  // Zone 1 run / stop / long-press fault-clear
constexpr uint8_t BTN2 = 35;  // Zone 2 run / stop / long-press fault-clear
constexpr uint8_t BTN3 = 39;  // Zone 3 run / stop / long-press fault-clear

// Button LED rings — 24V, one low-side FET per ring (active-high to the FET gate).
constexpr uint8_t LED1 = 32;
constexpr uint8_t LED2 = 33;
constexpr uint8_t LED3 = 23;

// Watchdog handshake (ATtiny85).
// HEARTBEAT_OUT carries a square wave emitted ONLY while a run is active
// (DEC-004): "heartbeat present" == "a run is in progress". No separate
// run-active line exists — the wiring doc allocates none.
constexpr uint8_t HEARTBEAT_OUT = 4;
constexpr uint8_t WD_TRIPPED_IN = 36;  // ATtiny "tripped" -> ESP32 (input-only). For logging/display.
                                       // ACTIVE LOW: the 10k pull-up idles it HIGH; the ATtiny
                                       // emulates open-drain (drives LOW = tripped, Hi-Z = released)
                                       // so a 5V ATtiny can't overvolt this pin and an absent
                                       // watchdog reads "not tripped" (the relay is the safety).

// Free for more zones (build-for-three): 19, 21, 5, 2, 12, 15.

// Zones modeled as a table so the count is data-driven (firmware spec §3).
struct Zone { uint8_t fetPin, ledPin, btnPin; const char* name; };
constexpr Zone ZONES[] = {
    { Z1_FET, LED1, BTN1, "Zone 1" },
    { Z2_FET, LED2, BTN2, "Zone 2" },
    { Z3_FET, LED3, BTN3, "Zone 3" },   // hose outlet — wired now, plumbed later
};
constexpr uint8_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
