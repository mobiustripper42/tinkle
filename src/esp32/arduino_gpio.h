#pragma once
#include <Arduino.h>
#include "../core/valve_driver.h"
#include "../core/buttons.h"

// ESP32 bindings for the platform-independent GPIO interfaces. Header-only — the
// only place pin I/O touches the Arduino API. RunController (§4) builds a
// ValveConfig from pins.h and drives actuators through ArduinoGpio; Buttons (§11)
// reads the panel through ArduinoButtonInput.

namespace tinkle {

struct ArduinoGpio : IGpio {
    void configureOutput(uint8_t pin) override { pinMode(pin, OUTPUT); }
    void write(uint8_t pin, bool high) override { digitalWrite(pin, high ? HIGH : LOW); }
};

// Buttons are momentary-to-GND with an external pull-up (§11 / wiring), so a pin
// reads LOW when pressed. INPUT (not INPUT_PULLUP): GPIO34/35/39 are input-only and
// have no internal pull-ups — the pull-up is external. isDown() folds in the
// active-low inversion so the debounce logic stays polarity-agnostic.
struct ArduinoButtonInput : IButtonInput {
    void configureInput(uint8_t pin) override { pinMode(pin, INPUT); }
    bool isDown(uint8_t pin)         override { return digitalRead(pin) == LOW; }
};

} // namespace tinkle
