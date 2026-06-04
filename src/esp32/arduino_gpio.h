#pragma once
#include <Arduino.h>
#include "../core/valve_driver.h"

// ESP32 binding for the platform-independent IGpio sink. Header-only — the only
// place ValveDriver's pin I/O touches the Arduino API. RunController (§4) builds
// a ValveConfig from pins.h, hands it this adapter, and drives the actuators.

namespace tinkle {

struct ArduinoGpio : IGpio {
    void configureOutput(uint8_t pin) override { pinMode(pin, OUTPUT); }
    void write(uint8_t pin, bool high) override { digitalWrite(pin, high ? HIGH : LOW); }
};

} // namespace tinkle
