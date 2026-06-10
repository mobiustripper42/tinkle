#pragma once
#include <stdint.h>

// ValveDriver — low-level actuation for Tinkle (firmware spec §5).
//
// Platform-independent on purpose: it lives in src/core so it compiles for both
// the ESP32 firmware and the native host test runner. It touches no Arduino API
// directly — all pin I/O goes through an injected IGpio. The ESP32 build wraps
// digitalWrite/pinMode (src/esp32/arduino_gpio.h); the host test runner records
// writes.
//
// v1.4 model (DEC-011/012/013): every valve is a 2-wire on/off motorized ball
// valve driven by a SINGLE low-side FET — one GPIO each. Energize the FET to
// actuate (the valve travels ~6-10 s, self-cuts at its limit, then holds open
// while energized); de-energize and a capacitor auto-returns it to rest. There is
// no master valve — the pump on the armed 24V is the source gate (DEC-012) — and
// no H-bridge / never-both-high invariant. The Dosatron diverter is two 2-way legs
// (a NO clean/bypass leg + a NC fert leg) driven together (DEC-013).
//
// Per §4, RunController is the ONLY module allowed to command these — everything
// else requests runs through it.

namespace tinkle {

// Platform-independent GPIO sink. Outputs only; ValveDriver never reads a pin.
struct IGpio {
    virtual void configureOutput(uint8_t pin) = 0;
    virtual void write(uint8_t pin, bool high) = 0;
    virtual ~IGpio() = default;
};

// Static wiring + timing the driver needs. Built from pins.h on the ESP32, from
// synthetic pins in tests. Sized for three tunnels (build-for-three).
struct ValveConfig {
    static constexpr uint8_t MAX_ZONES = 3;

    uint8_t  zoneFet[MAX_ZONES] = {};   // one low-side FET per NC zone valve
    uint8_t  zoneCount          = 0;
    uint8_t  divCleanFet        = 0;    // NO bypass leg  (de-energized = open  = plain)
    uint8_t  divFertFet         = 0;    // NC Dosatron leg (de-energized = closed)
    uint8_t  pumpRelay          = 0;

    // Travel windows (§15 — bench-confirm against real parts; datasheet 6-10 s). A
    // 2-wire ball valve drives full travel; there is no 75 ms latch pulse anymore.
    // TINKLE_SIM (the Wokwi build, platformio.ini [env:esp32_sim]) shortens these so a
    // run is watchable — they are NOT the real-hardware values. Wet/bench builds use §15.
#ifdef TINKLE_SIM
    uint16_t zoneTravelMs     = 1000;
    uint16_t diverterTravelMs = 1000;
#else
    uint16_t zoneTravelMs     = 10000;  // §15 ZONE_TRAVEL_MS
    uint16_t diverterTravelMs = 10000;  // §15 DIVERTER_TRAVEL_MS
#endif
};

class ValveDriver {
public:
    ValveDriver(IGpio& gpio, const ValveConfig& cfg);

    // Configure every FET pin as an output and force the safe/rest levels: all FETs
    // off — NC zones closed, NC fert-leg closed, NO bypass open (plain), pump off.
    // Call once at boot.
    void begin();

    // Zone valves (NC). openZone energizes the FET — the valve drives open and holds
    // open while energized; closeZone de-energizes — the capacitor auto-return drives
    // it closed. Either transition takes ~zoneTravelMs; zoneBusy() gates the §4
    // sequence until then. Out-of-range zone is a no-op.
    void openZone(uint8_t zone, uint32_t nowMs);
    void closeZone(uint8_t zone, uint32_t nowMs);

    // Diverter — two legs driven together (DEC-013). fertigate => both leg FETs HIGH
    // (NC fert-leg opens, NO bypass closes); !fertigate => both LOW (rest: bypass
    // open, fert-leg closed). Holds diverterTravelMs busy. No NVS cache — the rest
    // state is defined by the NO/NC valve types, so the boot/rest position is known
    // to be plain (fertigate=false).
    void setDiverter(bool fertigate, uint32_t nowMs);

    // Pump relay (the source gate, DEC-012) — immediate level set, no travel window.
    void pumpOn();
    void pumpOff();

    // Safe state (§5/§14): pump off first, then de-energize every valve FET — zones
    // cap-close, the diverter returns to plain. Commanded immediately (no travel
    // gating: the caller either latches FAULT or boots to IDLE). The fail-dry barrier
    // is pump-off (the source), not the valves.
    void safeState(uint32_t nowMs);

    // Cooperative, non-blocking. Call every loop tick; clears expired travel timers.
    // The FET levels themselves HOLD — an open valve stays energized; tick() only
    // releases the busy flag once travel completes. Never blocks.
    void tick(uint32_t nowMs);

    // Introspection for status/display/tests.
    uint8_t zoneCount() const { return cfg_.zoneCount; }
    bool zoneBusy(uint8_t zone) const;
    bool diverterBusy() const;
    bool busy() const;                            // any actuator mid-travel
    bool pumpIsOn()        const { return pumpOn_; }
    bool diverterFert()    const { return diverterFert_; }   // commanded leg state

private:
    struct Timer { bool active; uint32_t startMs; uint16_t durMs; };

    IGpio&      g_;
    ValveConfig cfg_;
    Timer       zoneTimer_[ValveConfig::MAX_ZONES] = {};
    Timer       divTimer_     = {};
    bool        pumpOn_       = false;
    bool        diverterFert_ = false;   // commanded: false = plain (boot/rest state)
};

} // namespace tinkle
