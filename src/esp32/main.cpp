#include <Arduino.h>
#include "pins.h"

// Tinkle ESP32 firmware — entry point.
//
// SCAFFOLD ONLY. On boot this drives every actuator to the fail-dry safe state
// and idles. The cooperative non-blocking loop and the modules it drives
// (RunController, Scheduler, FlowMonitor, WebConfig, Watchdog, ...) land in
// Phase 1+ per docs/tinkle_firmware_spec.md.

static void safeState() {
    // Master closed, pump off, all H-bridge inputs low (latching valves hold
    // state with zero current between pulses).
    pinMode(MASTER_FET, OUTPUT); digitalWrite(MASTER_FET, LOW);
    pinMode(PUMP_RELAY, OUTPUT); digitalWrite(PUMP_RELAY, LOW);
    const uint8_t bridge[] = { Z1_IN1, Z1_IN2, Z2_IN1, Z2_IN2, DIV_IN1, DIV_IN2 };
    for (uint8_t p : bridge) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
}

void setup() {
    Serial.begin(115200);
    safeState();
    // Heartbeat is emitted only during active runs (DEC-004) — idle low.
    pinMode(HEARTBEAT_OUT, OUTPUT); digitalWrite(HEARTBEAT_OUT, LOW);
    pinMode(WD_TRIPPED_IN, INPUT);
}

void loop() {
    // Cooperative, fully non-blocking. Target tick <= 10 ms. No delay() here.
}
