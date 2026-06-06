#include <Arduino.h>
#include "pins.h"
#include "arduino_gpio.h"
#include "../core/valve_driver.h"
#include "../core/run_controller.h"
#include "../core/loop_monitor.h"
#include "../core/buttons.h"
#include "../core/display.h"
#include "../core/persistence.h"
#include "../core/clock.h"
#include "../core/scheduler.h"
#include "display_tm1637.h"
#include "preferences_store.h"
#include "system_clock.h"

// Tinkle ESP32 firmware — entry point and the cooperative non-blocking loop
// (firmware spec §2). On boot RunController::begin forces the fail-dry safe state
// through ValveDriver (pump off -> zones closed -> master closed) and parks the
// actuation core at IDLE. The loop then ticks that core every pass off a single
// millis() read, with no delay() anywhere, and instruments each tick against the
// 10 ms budget.
//
// RunController is the sole actuator commander; the loop ticks it (which ticks the
// ValveDriver in turn) and ticks Buttons (§11), mapping debounced edges onto run
// requests, then renders the TM1637 panel + LED rings (§12). The scheduler / flow /
// web / watchdog modules (Phase 2+) hang off this same loop as they land — each is
// one more non-blocking tick().

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

// Persistence (§8 / DEC-008). Owns the per-zone manual default durations, swMaxRuntimeSec,
// and cached diverter position in NVS, keyed for forward-compat as zones grow. begin()
// runs in setup() after the store opens; reads fall back to defaults on empty NVS.
static PreferencesStore  prefsStore;
static Persistence       persistence(prefsStore, ZONE_COUNT);

// Clock (§13 / DEC-009). NTP sync + free-running millis() fallback behind the SystemClock
// shim (the only SNTP/timezone code). valid() drives the display's clockValid; until NTP
// lands (no WiFi before Phase 4) it stays false and the panel holds "--:--".
static SystemClock       systemClock;
static Clock             wallClock(systemClock);

// Scheduler (§13). Evaluates schedule entries once per local minute and enqueues due runs
// through RunController (the sole actuator commander), applying the §6 fert policy. The
// schedule is empty until the web-config editor lands (Phase 4); with no clock yet (no WiFi
// pre-Phase-4) tick() is a no-op regardless.
static Scheduler         scheduler(runController, wallClock);

// Button panel (§11 / DEC-006): one button per zone, no dedicated stop button. Pins
// come straight from the pins.h zone table (data-driven). The press policy lives in
// loop(); the panel module only produces clean debounced edges.
static Buttons::Config makeButtonConfig() {
    static_assert(ZONE_COUNT <= Buttons::MAX_BUTTONS,
                  "button panel: one button per zone must fit MAX_BUTTONS");
    Buttons::Config cfg;
    for (uint8_t i = 0; i < ZONE_COUNT; ++i) cfg.pins[i] = ZONES[i].btnPin;  // one per zone
    cfg.count = ZONE_COUNT;
    return cfg;
}

static ArduinoButtonInput   btnInput;
static const Buttons::Config btnCfg = makeButtonConfig();
static Buttons              buttons(btnInput, btnCfg);

// Fault-clear success ack (DEC-006 / §12): when a long-press clearFault() takes, flash
// every ring solid for this long so a held button never reads as a dead panel. The
// complementary "held while latched-but-UNRESOLVED -> visible no-op" cue is gated on
// the FaultManager resolved-condition signal (Phase 3/5); clearFault() today clears
// unconditionally when faulted, so only the success ack ("was faulted, now cleared")
// can fire now — it does NOT mean "the fault condition is gone".
static constexpr uint32_t FAULT_ACK_MS  = 750;
static uint32_t           faultAckUntilMs = 0;

static DisplayTM1637 display;            // §12 panel; pushes only on content change

// LED ring level (§11/§12): Solid -> on; Blink -> on during the blink phase; Off.
static bool ledLevel(LedMode m, bool blinkOn) {
    return m == LedMode::Solid || (m == LedMode::Blink && blinkOn);
}

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

    // Open NVS and load stored config (§8 / DEC-008) before the loop can act on it:
    // per-zone default durations feed manual-button runs; empty NVS falls back to defaults.
    prefsStore.begin();
    persistence.begin();

    // Configure TZ + SNTP and take the first sync attempt (§13). No network yet, so this
    // stays invalid until Phase 4 brings up WiFi; the call is harmless until then.
    systemClock.begin();
    wallClock.begin(millis());

    buttons.begin();              // configure the panel pins as inputs (§11)
    display.begin();              // TM1637 init + clear (§12)

    // Button LED rings (§11/§12) are outputs (active-high via ULN2803); off at boot.
    // ZONE_COUNT now covers all three rings (LED3 is Zone 3's), so no separate handling.
    for (uint8_t z = 0; z < ZONE_COUNT; ++z) { pinMode(ZONES[z].ledPin, OUTPUT); digitalWrite(ZONES[z].ledPin, LOW); }

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
    wallClock.tick(now);       // poll NTP / free-run the wall clock (§13)
    scheduler.tick(now);       // per-minute eval of due runs -> RunController (§13)

    // §11 manual buttons (DEC-006). RunController owns the single-active invariant; we
    // only map debounced edges onto it. One uniform policy for every zone button:
    //   FAULT      -> short press is a no-op (only the long-press clears);
    //   any run on -> any press STOPS it (no switching, no auto-start);
    //   IDLE       -> press starts that button's own zone.
    // A >=3 s long-press of any button clears a latched fault. A long hold fires
    // pressEdge first, but in FAULT that press is a no-op, so it harmlessly precedes
    // the clear 3 s later. clearFault() returns false unless actually faulted, so the
    // ack (and the clear) only fire from a genuine latched fault.
    for (uint8_t z = 0; z < ZONE_COUNT; ++z) {
        if (buttons.pressEdge(z)) {
            if (runController.isFaulted()) {
                /* short press in FAULT: no-op (§11 / DEC-006) */
            } else if (!runController.isIdle()) {
                runController.stop(now);                  // any zone running -> stop
            } else {
                RunRequest req;
                req.zoneIndex   = z;
                req.durationSec = persistence.zoneDefaultSec(z);   // stored per-zone default (§8)
                req.fertigate   = false;
                runController.requestRun(req, now);       // idle -> start this zone
            }
        }
        if (buttons.longPressEdge(z) && runController.clearFault())
            faultAckUntilMs = now + FAULT_ACK_MS;         // §12 success ack
    }

    // §12 panel. Render the frame from controller state; the shim pushes to the
    // TM1637 only when it changes (bit-bang cost vs the tick budget). The idle clock
    // shows HH:MM once NTP syncs, "--:--" until then (§13).
    const WallTime wt = wallClock.wall(now);
    DisplayInputs di;
    di.state        = runController.state();
    di.fault        = runController.activeFault();
    di.remainingSec = runController.remainingSec(now);
    di.clockValid   = wallClock.valid();
    di.hours        = wt.hour;
    di.minutes      = wt.minute;
    display.show(renderDisplay(di, now));

    // LED rings (DEC-006): active zone solid; ALL rings blink on fault (no dedicated
    // stop ring); ALL rings flash solid briefly after a successful fault-clear (§12
    // ack). digitalWrite is cheap, so drive every loop. Same 1 Hz phase as the display
    // colon (one stretched blink at the ~49.7-day millis wrap — cosmetic). The ack
    // window comparison is millis()-rollover safe via the signed difference.
    const bool blinkOn = (now / 500u) % 2u == 0u;
    const bool ackOn   = (int32_t)(faultAckUntilMs - now) > 0;
    const int  az      = runController.activeZone();
    const bool faulted = runController.isFaulted();
    for (uint8_t z = 0; z < ZONE_COUNT; ++z) {
        const LedMode m = ackOn ? LedMode::Solid : zoneLedMode(az, z, faulted);
        digitalWrite(ZONES[z].ledPin, ledLevel(m, blinkOn) ? HIGH : LOW);
    }

    // (flow / web / watchdog tick here as they land)

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
