#pragma once
#include <stdint.h>

// Tinkle ESP32 GPIO map. Mirrors docs/tinkle_wiring.html (§B) — the wiring doc is
// the source of truth for pins; if this disagrees with it, the wiring doc wins.
//
// ESP32 DevKitC 38-pin. Avoids flash pins (6-11) and TX/RX (1/3). v1.4 (DEC-011):
// one low-side FET per valve (1 GPIO each), no H-bridges — so no strapping pins are
// needed and the DEC-007 GPIO12/15 brick-boot contortion is retired. GPIO 34/35/36/39
// are input-only (no internal pull-ups); only the watchdog-trip line (36) is still used
// there — external 10k pull-up + 100nF cap. v1.5 (DEC-019) is phone-only: the TM1637
// display, the three zone buttons (34/35/39), and the three button LED rings are gone,
// banking those GPIO for future zones. The SPA is the sole interface.

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

// Alive / board-health LED (DEC-019). The DevKitC's onboard LED — no external part. The
// loop blinks it at ~1 Hz so a glance confirms the firmware is still ticking; it is the
// only on-board status indicator now that the TM1637 + button rings are gone. Purely
// cosmetic — it gates nothing (status proper lives in the SPA, §10.1). NOTE: distinct
// from HEARTBEAT_OUT (GPIO4), the ATtiny watchdog heartbeat — this LED is local health,
// not the safety handshake.
constexpr uint8_t ALIVE_LED = 2;

// Watchdog handshake (ATtiny85).
// HEARTBEAT_OUT carries a square wave emitted ONLY while a run is active
// (DEC-004): "heartbeat present" == "a run is in progress". No separate
// run-active line exists — the wiring doc allocates none.
constexpr uint8_t HEARTBEAT_OUT = 4;
constexpr uint8_t WD_TRIPPED_IN = 36;  // ATtiny "tripped" -> ESP32 (input-only). For logging / SPA status.
                                       // ACTIVE LOW: the 10k pull-up idles it HIGH; the ATtiny
                                       // emulates open-drain (drives LOW = tripped, Hi-Z = released)
                                       // so a 5V ATtiny can't overvolt this pin and an absent
                                       // watchdog reads "not tripped" (the relay is the safety).

// Free for more zones (build-for-three). The original spares plus everything DEC-019
// freed: 21, 5, 12, 15 (and 19, used only by the TINKLE_SIM flow loopback); 25, 26, 23,
// 32, 33 (ex-display / ex-LED-ring outputs); 34, 35, 39 (ex-buttons, input-only).

// Zones modeled as a table so the count is data-driven (firmware spec §3). v1.5 drops
// the per-zone LED/button columns (DEC-019) — a zone is now just its valve FET + name.
struct Zone { uint8_t fetPin; const char* name; };
constexpr Zone ZONES[] = {
    { Z1_FET, "Zone 1" },
    { Z2_FET, "Zone 2" },
    { Z3_FET, "Zone 3" },   // hose outlet — wired now, plumbed later
};
constexpr uint8_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
