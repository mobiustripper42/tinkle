#pragma once
#include <stdint.h>

// Buttons — debounce + edge detection for the manual panel (firmware spec §11).
//
// Platform-independent (src/core): driven by an explicit millis() and an injected
// input source, host-tested with a fake clock + fake inputs. It produces clean
// edges; it does NOT command anything. The run policy (which edge starts/stops a
// run or clears a fault, and the single-active invariant) lives with the consumer
// (main.cpp), since RunController is the sole actuator commander (§4).
//
// Software debounce (~30 ms, §11) layered on the RC hardware debounce; acts on the
// press edge; flags a long press (≥ longPressMs) once per hold for B3 fault-clear.

namespace tinkle {

// Platform-independent button input source. isDown() is logical: true = pressed.
// The ESP32 adapter inverts the active-low pin read (momentary to GND + external
// pull-up, §11 / wiring), so this debounce logic never deals in pin polarity.
struct IButtonInput {
    virtual void configureInput(uint8_t pin) = 0;
    virtual bool isDown(uint8_t pin)         = 0;
    virtual ~IButtonInput() = default;
};

class Buttons {
public:
    static constexpr uint8_t MAX_BUTTONS = 3;

    struct Config {
        uint8_t  pins[MAX_BUTTONS] = {};
        uint8_t  count             = 0;
        uint16_t debounceMs        = 30;     // §11 ~30 ms software debounce
        uint16_t longPressMs       = 3000;   // §11 ≥3 s hold = B3 fault-clear
    };

    Buttons(IButtonInput& in, const Config& cfg);

    void begin();                 // configure every button pin as an input
    void tick(uint32_t nowMs);    // debounce + edge detect; cooperative, non-blocking

    // Edge flags are transient: each reflects only the tick() in which the edge
    // occurred and clears on the next tick(). Poll them right after tick().
    bool pressEdge(uint8_t i)     const;  // debounced press transition, this tick
    bool releaseEdge(uint8_t i)   const;  // debounced release transition, this tick
    bool longPressEdge(uint8_t i) const;  // crossed longPressMs while held, once per hold
    bool isDown(uint8_t i)        const;  // current debounced state

private:
    struct State {
        bool     stable        = false;   // debounced logical state
        bool     lastRaw       = false;   // last raw read, for bounce rejection
        uint32_t lastChangeMs  = 0;       // when the raw read last changed
        uint32_t pressedAtMs   = 0;       // when stable last went down (for long press)
        bool     longFired     = false;   // long press already emitted this hold
        bool     pressEdge     = false;
        bool     releaseEdge   = false;
        bool     longPressEdge = false;
    };

    IButtonInput& in_;
    Config        cfg_;
    State         st_[MAX_BUTTONS] = {};
};

} // namespace tinkle
