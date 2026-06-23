#pragma once
#include <Arduino.h>
#include "../core/valve_driver.h"

// ESP32 binding for the platform-independent GPIO interface. Header-only — the only
// place pin I/O touches the Arduino API. RunController (§4) builds a ValveConfig from
// pins.h and drives actuators through ArduinoGpio. (v1.5 / DEC-019 dropped the button
// panel, so the former ArduinoButtonInput binding is gone with it.)

namespace tinkle {

struct ArduinoGpio : IGpio {
    void configureOutput(uint8_t pin) override { pinMode(pin, OUTPUT); }
    void write(uint8_t pin, bool high) override { digitalWrite(pin, high ? HIGH : LOW); }
};

} // namespace tinkle
