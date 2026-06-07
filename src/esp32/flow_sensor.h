#pragma once
#include <Arduino.h>
#include "pins.h"

// ESP32 binding for the flow sensor — the only place pulse counting touches the Arduino
// ISR API. Header-only, mirroring arduino_gpio.h / preferences_store.h. The hall sensor on
// FLOW_PIN (level-shifted to 3.3 V, §3) emits a pulse per unit volume; the ISR just
// increments a counter. FlowMonitor (src/core) consumes the cumulative count via pulses().
//
// Instance-based, not a global counter: attachInterruptArg hands the ISR a `this` pointer,
// so the counter is a plain member (no out-of-line static needed to stay header-only).

namespace tinkle {

struct FlowSensor {
    volatile uint32_t count = 0;

    static void IRAM_ATTR isr(void* arg) { ++static_cast<FlowSensor*>(arg)->count; }

    void begin() {
        pinMode(FLOW_PIN, INPUT);   // external level-shift provides the signal; no internal pull
        attachInterruptArg(digitalPinToInterrupt(FLOW_PIN), isr, this, RISING);
    }

    // Cumulative pulse count. A 32-bit load is atomic on the ESP32, so no critical section is
    // needed — a torn read is impossible and a concurrent increment just lands on the next tick.
    uint32_t pulses() const { return count; }
};

} // namespace tinkle
