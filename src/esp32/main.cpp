#include <Arduino.h>
#include "pins.h"
#include "arduino_gpio.h"
#include "../core/valve_driver.h"
#include "../core/run_controller.h"
#include "../core/loop_monitor.h"
#include "../core/buttons.h"

// Tinkle ESP32 firmware — entry point and the cooperative non-blocking loop
// (firmware spec §2). On boot RunController::begin forces the fail-dry safe state
// through ValveDriver (pump off -> zones closed -> master closed) and parks the
// actuation core at IDLE. The loop then ticks that core every pass off a single
// millis() read, with no delay() anywhere, and instruments each tick against the
// 10 ms budget.
//
// RunController is the sole actuator commander; the loop ticks it (which ticks the
// ValveDriver in turn) and ticks Buttons (§11), mapping debounced edges onto run
// requests. Display (§12, #14) and the scheduler / flow / web / watchdog modules
// (Phase 2+) hang off this same loop as they land — each is one more non-blocking
// tick().

using namespace tinkle;

// ValveConfig assembled from the pins.h zone table, so the zone count stays
// data-driven (§3). ZONE_COUNT is <= ValveConfig::MAX_ZONES (build-for-three).
static ValveConfig makeValveConfig() {
    ValveConfig cfg;
    cfg.zoneCount = ZONE_COUNT;
    for (uint8_t i = 0; i < ZONE_COUNT; ++i)
        cfg.zones[i] = Bridge{ ZONES[i].in1, ZONES[i].in2 };
    cfg.diverter  = Bridge{ DIV_IN1, DIV_IN2 };   // in1 = THROUGH (fertigate), in2 = AROUND
    cfg.masterFet = MASTER_FET;
    cfg.pumpRelay = PUMP_RELAY;
    return cfg;
}

static ArduinoGpio      gpio;
static const ValveConfig valveCfg = makeValveConfig();
static ValveDriver       valve(gpio, valveCfg);
static const RunConfig   runCfg;                  // firmware spec §15 defaults
static RunController      runController(valve, runCfg);

// Button panel (§11): one button per zone (B1/B2) plus B3 stop/cancel + fault-clear.
// Pins come from the pins.h zone table (data-driven) and BTN3.
static Buttons::Config makeButtonConfig() {
    static_assert(ZONE_COUNT + 1 <= Buttons::MAX_BUTTONS,
                  "button panel: one button per zone + one stop must fit MAX_BUTTONS");
    Buttons::Config cfg;
    for (uint8_t i = 0; i < ZONE_COUNT; ++i) cfg.pins[i] = ZONES[i].btnPin;  // B1/B2 -> zones
    cfg.pins[ZONE_COUNT] = BTN3;                                             // B3  -> stop/clear
    cfg.count            = ZONE_COUNT + 1;
    return cfg;
}

static ArduinoButtonInput   btnInput;
static const Buttons::Config btnCfg = makeButtonConfig();
static Buttons              buttons(btnInput, btnCfg);

static constexpr uint8_t  STOP_BTN_IDX   = ZONE_COUNT;   // B3 sits just past the zone buttons
// Placeholder default run length for a manual press. The per-zone stored default
// (§11) comes from Persistence (§8) once #2.1 lands; until then every button run
// uses this.
static constexpr uint32_t BUTTON_RUN_SEC = 600;

// §2 tick budget. micros() resolution so sub-millisecond ticks still register.
static constexpr uint32_t TICK_BUDGET_US     = 10000;   // 10 ms
static constexpr uint32_t REPORT_INTERVAL_MS = 5000;
static LoopMonitor        loopMon(TICK_BUDGET_US);
static uint32_t           lastReportMs = 0;

void setup() {
    Serial.begin(115200);

    // Safe state before anything can command water, then arm the core at IDLE.
    // begin() configures every actuator pin as an output and drives the fail-dry
    // safe sequence through ValveDriver (pump off -> zones closed -> master closed).
    runController.begin(millis());
    buttons.begin();              // configure the panel pins as inputs (§11)

    // Watchdog handshake pins (§9). The heartbeat is emitted ONLY during active
    // runs (DEC-004) and the trip line is consumed by the Watchdog module — both
    // land in Phase 5 (#5.2). Until then: heartbeat idle low, trip line readable.
    pinMode(HEARTBEAT_OUT, OUTPUT); digitalWrite(HEARTBEAT_OUT, LOW);
    pinMode(WD_TRIPPED_IN, INPUT);

    Serial.println(F("[tinkle] boot: safe state, IDLE — cooperative loop running."));
}

void loop() {
    // One millis() read per pass; long actions time against it inside the modules.
    const uint32_t now = millis();
    const uint32_t t0  = micros();

    runController.tick(now);   // sole actuator commander; ticks the ValveDriver too
    buttons.tick(now);         // debounce + edge detect (§11)

    // §11 manual buttons. RunController owns the single-active invariant; we only map
    // debounced edges onto it. B1/B2 press: start that zone if idle, else cancel the
    // other run and start it (queued behind the unwind). Same-zone press is a no-op.
    for (uint8_t z = 0; z < ZONE_COUNT; ++z) {
        if (buttons.pressEdge(z) && runController.activeZone() != (int)z) {
            runController.stop(now);
            RunRequest req;
            req.zoneIndex   = z;
            req.durationSec = BUTTON_RUN_SEC;
            req.fertigate   = false;
            runController.requestRun(req, now);
        }
    }
    // B3: short press = stop/cancel-all (no-op when idle/faulted); long press (≥3 s)
    // = fault-clear. A long hold fires pressEdge first too, but stop() is a no-op in
    // FAULT, so the harmless stop simply precedes the clear.
    if (buttons.pressEdge(STOP_BTN_IDX))     runController.stop(now);
    if (buttons.longPressEdge(STOP_BTN_IDX)) runController.clearFault();

    // (display / scheduler / flow / web / watchdog tick here as they land)

    // micros() subtraction wraps cleanly across the ~71 min rollover.
    if (loopMon.record(micros() - t0))
        Serial.printf("[tinkle] tick overrun: %lu us > %lu us budget\n",
                      (unsigned long)loopMon.stats().lastUs,
                      (unsigned long)TICK_BUDGET_US);

    if (now - lastReportMs >= REPORT_INTERVAL_MS) {
        lastReportMs = now;
        const LoopStats& s = loopMon.stats();
        Serial.printf("[tinkle] state=%u tick last=%lu us max=%lu us overruns=%lu/%lu\n",
                      (unsigned)runController.state(),
                      (unsigned long)s.lastUs, (unsigned long)s.maxUs,
                      (unsigned long)s.overruns, (unsigned long)s.ticks);
    }
}
