#pragma once
#include <stdint.h>

// ValveDriver — low-level actuation for Tinkle (firmware spec §5).
//
// Platform-independent on purpose: it lives in src/core so it compiles for both
// the ESP32 firmware and the native host test runner. It touches no Arduino API
// directly — all pin I/O goes through an injected IGpio. The ESP32 build wraps
// digitalWrite/pinMode (src/esp32/arduino_gpio.h); the host test runner records
// writes and polices the never-both-high invariant.
//
// Drives latching zone valves (pulse open/close, then coast), the motorized
// Dosatron diverter (timed travel), the master FET, and the pump relay. Per §4,
// RunController is the ONLY module allowed to command these — everything else
// requests runs through it.

namespace tinkle {

// Platform-independent GPIO sink. Outputs only; ValveDriver never reads a pin.
struct IGpio {
    virtual void configureOutput(uint8_t pin) = 0;
    virtual void write(uint8_t pin, bool high) = 0;
    virtual ~IGpio() = default;
};

// One DRV8871 H-bridge driving a latching valve. IN1 = open, IN2 = close.
// The never-both-high invariant is a property of how this pair is driven.
struct Bridge { uint8_t in1; uint8_t in2; };

// Static wiring + timing the driver needs. Built from pins.h on the ESP32,
// from synthetic pins in tests. Sized for three tunnels (build-for-three).
struct ValveConfig {
    static constexpr uint8_t MAX_ZONES = 3;

    Bridge   zones[MAX_ZONES] = {};
    uint8_t  zoneCount        = 0;
    Bridge   diverter         = {};   // in1 = THROUGH (fertigate), in2 = AROUND (plain)
    uint8_t  masterFet        = 0;
    uint8_t  pumpRelay        = 0;

    uint16_t pulseMs          = 75;    // §15 PULSE_MS — bench-confirm against real parts
    uint16_t diverterTravelMs = 6000;  // §15 DIVERTER_TRAVEL_MS — bench-confirm
};

class ValveDriver {
public:
    ValveDriver(IGpio& gpio, const ValveConfig& cfg);

    // Configure every pin as an output and force the safe levels: bridges coast,
    // master closed, pump off. Call once at boot.
    void begin();

    // Latching zone valves. Start a pulse now; tick() releases to coast after
    // pulseMs. Out-of-range zone is a no-op. Re-issuing before the pulse expires
    // re-drives the bridge and restarts the timer (one pulse per intent — the
    // caller, RunController, owns de-duplication).
    void pulseOpen(uint8_t zone, uint32_t nowMs);
    void pulseClose(uint8_t zone, uint32_t nowMs);

    // Motorized diverter. through=true => fertigate (THROUGH). Drives for
    // diverterTravelMs, then coasts; the valve self-stops at its limit. The
    // commanded position is cached in memory here; persisting it to NVS is
    // Persistence's job (§8).
    void setDiverter(bool through, uint32_t nowMs);

    // Master FET + pump relay — immediate level sets, no travel window.
    void masterOpen();
    void masterClose();
    void pumpOn();
    void pumpOff();

    // Safe state (§5/§14): pump off -> zones close-pulsed -> master closed.
    // Diverter is left as-is. Commanded immediately; the close pulses complete
    // over pulseMs via tick().
    void safeState(uint32_t nowMs);

    // Cooperative, non-blocking. Call every loop tick; releases expired pulses
    // and the diverter travel window. Never blocks.
    void tick(uint32_t nowMs);

    // Introspection for status/display/tests.
    bool zoneBusy(uint8_t zone) const;
    bool diverterBusy() const;
    bool busy() const;                            // any actuator mid-travel
    bool masterIsOpen()    const { return masterOpen_; }
    bool pumpIsOn()        const { return pumpOn_; }
    bool diverterThrough() const { return diverterThrough_; }
    bool diverterKnown()   const { return diverterKnown_; }

private:
    // Both-high is unrepresentable: a bridge is only ever Coast, Open, or Close.
    enum class Cmd : uint8_t { Coast, Open, Close };

    struct Timer { bool active; uint32_t startMs; uint16_t durMs; };

    void applyBridge(const Bridge& b, Cmd cmd);   // writes LOW side first => no transient both-high

    IGpio&      g_;
    ValveConfig cfg_;
    Timer       zoneTimer_[ValveConfig::MAX_ZONES] = {};
    Timer       divTimer_       = {};
    bool        masterOpen_     = false;
    bool        pumpOn_         = false;
    bool        diverterThrough_ = false;   // commanded position (cache to NVS in Persistence)
    bool        diverterKnown_  = false;
};

} // namespace tinkle
