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
#include "../core/flow_monitor.h"
#include "../core/flow_fault_detector.h"
#include "../core/calibration_controller.h"
#include "../core/watchdog.h"
#include "../core/fault_manager.h"
#include "../core/valve_rest_monitor.h"
#include "../core/api.h"
#include "display_tm1637.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "preferences_store.h"
#include "system_clock.h"
#include "flow_sensor.h"

// Tinkle ESP32 firmware — entry point and the cooperative non-blocking loop
// (firmware spec §2). On boot RunController::begin forces the fail-dry safe state
// through ValveDriver (pump off -> all valve FETs de-energized -> diverter plain) and
// parks the actuation core at IDLE. The loop then ticks that core every pass off a single
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
        cfg.zoneFet[i] = ZONES[i].fetPin;         // one low-side FET per zone (v1.4, DEC-011)
    cfg.divCleanFet = DIV_CLEAN_FET;              // NO bypass leg (DEC-013)
    cfg.divFertFet  = DIV_FERT_FET;              // NC Dosatron leg
    cfg.pumpRelay   = PUMP_RELAY;                // the source gate (DEC-012); no master
    return cfg;
}

static ArduinoGpio      gpio;
static const ValveConfig valveCfg = makeValveConfig();
static ValveDriver       valve(gpio, valveCfg);
static const RunConfig   runCfg;                  // firmware spec §15 defaults
static RunController      runController(valve, runCfg);

// Persistence (§8 / DEC-008). Owns the per-zone manual default durations and
// swMaxRuntimeSec in NVS, keyed for forward-compat as zones grow. begin() runs in
// setup() after the store opens; reads fall back to defaults on empty NVS. (No cached
// diverter position — the two-leg diverter rests plain by construction, DEC-013.)
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

// Flow monitoring (§7 / #34). The ISR-backed counter (flow_sensor.h) is the only hardware
// touch; FlowMonitor turns its cumulative pulses into per-run gallons + a live GPM rate. K
// (pulses/gallon) loads from NVS at boot and is overwritten by calibration (#36). The §7
// no-flow / unexpected-flow faults (#35) hang off the same rate/accumulation.
static FlowSensor        flowSensor;
static FlowMonitor       flowMonitor(Persistence::DEFAULT_PULSES_PER_GALLON);

// Flow fault detection (§7/§14 / #35). Reads FlowMonitor's rate + the cumulative pulse
// count against the run state and pushes FAULT_NO_FLOW / FAULT_UNEXPECTED_FLOW through
// RunController::raiseFault (the sole commander), which slams the safe state and latches.
static const FlowFaultDetector::Config flowFaultCfg;   // §15 seeds
static FlowFaultDetector flowFault(flowFaultCfg);

// Calibration mode (§7 / #36). State machine + K math in core; the bounded cal run
// routes through RunController (sole commander) and K lands in NVS + the live
// FlowMonitor. start/finish/cancel are driven by the Phase 4 calibrate endpoints —
// until the web API lands, nothing calls them and tick() is a cheap no-op.
static const CalibrationController::Config calCfg;      // §15 seeds
static CalibrationController calibration(runController, persistence, flowMonitor, calCfg);

// Watchdog handshake, ESP32 half (§9 / #5.2). Emits the heartbeat on HEARTBEAT_OUT
// ONLY while the pump is commanded (START_PUMP/RUNNING) — the ATtiny arms on the
// first edge and its quiet-line timeout disarms it between runs, so HARD_MAX_RUNTIME
// is a per-run ceiling. The trip line is read here (active-low, see pins.h) and the
// verdict routes through RunController::raiseFault like every other detector.
static const Watchdog::Config wdCfg;                     // §15 HEARTBEAT_MS
static Watchdog watchdog(gpio, HEARTBEAT_OUT, wdCfg);

// Fault surface (§14 / #5.3 software half): fault-log ring + the resolved-condition
// clear gate. Every clear path (button long-press now, /api/fault/clear in Phase 4)
// goes through requestClear(), which refuses while the active fault's underlying
// condition still holds — detectors push that truth in each tick below.
static FaultManager faultManager(runController);

// Auto-return self-test (DEC-014 / #52). Watches the post-close rest window on
// the flow meter and flags a zone valve that fails to rest closed — a maintenance
// heads-up (cap aging), NOT a safety layer (the pump-power gate is the barrier).
// Non-latching: a flag becomes a FaultManager::note() + serial line, never a
// raiseFault. flaggedMask() feeds the Phase 4 status API.
static const ValveRestMonitor::Config restCfg;           // §15 seeds
static ValveRestMonitor valveRest(restCfg);

// Web layer (§10 / Unit C, #55-#57). Api is core (host-tested policy); WebServer is
// the async-HTTP plumbing serializing against this loop via lock()/unlock(); the
// WiFi manager joins the mesh or falls back to SoftAP, with mDNS either way. WiFi
// has no role in watering (§17 local autonomy) — it only carries the phone UI.
static Api         api(Api::Deps{runController, scheduler, persistence, faultManager,
                                 calibration, flowMonitor, flowFault, valveRest, wallClock});
static WifiManager wifiManager(persistence);
static WebServer   webServer(api, flowSensor, wifiManager);

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

// Fault-clear success ack (DEC-006 / §12): when a long-press clear takes, flash every
// ring solid for this long so a held button never reads as a dead panel. The clear
// now routes through FaultManager::requestClear() (§14, #5.3), so a long-press on a
// latched-but-UNRESOLVED fault is refused — no ack fires, the visible no-op cue.
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
    // safe sequence through ValveDriver (pump off -> all valve FETs off -> diverter plain).
    runController.begin(millis());

    // Open NVS and load stored config (§8 / DEC-008) before the loop can act on it:
    // per-zone default durations feed manual-button runs; empty NVS falls back to defaults.
    prefsStore.begin();
    persistence.begin();

    // Apply the persisted scalars that modules captured defaults for (§10 settings):
    // the software run ceiling and the DEC-015 flow-check override.
    runController.setSwMaxRuntimeSec(persistence.swMaxRuntimeSec());
    flowFault.setMuted(persistence.flowOverride());

    // Rehydrate the schedule from its NVS blob (§13 — the schedule lives in flash
    // and runs headless; WiFi is never required for it).
    {
        ScheduleEntry stored[Scheduler::MAX_ENTRIES];
        const uint8_t n = persistence.loadScheduleEntries(stored, Scheduler::MAX_ENTRIES);
        for (uint8_t i = 0; i < n; ++i) scheduler.add(stored[i]);
        if (n) Serial.printf("[tinkle] schedule: %u entries loaded\n", n);
    }

    // Configure TZ + SNTP and take the first sync attempt (§13). No network yet, so this
    // stays invalid until Phase 4 brings up WiFi; the call is harmless until then.
    systemClock.begin();
    wallClock.begin(millis());

    // Flow sensor ISR + monitor (§7). Load the calibrated K from NVS, then baseline the
    // counter so gallons starts at zero.
    flowSensor.begin();
    flowMonitor.setK(persistence.pulsesPerGallon());
    flowMonitor.begin(flowSensor.pulses(), millis());
    flowFault.begin(flowSensor.pulses(), millis());   // seed the idle window past boot

    buttons.begin();              // configure the panel pins as inputs (§11)
    display.begin();              // TM1637 init + clear (§12)

    // Button LED rings (§11/§12) are outputs (active-high via ULN2803); off at boot.
    // ZONE_COUNT now covers all three rings (LED3 is Zone 3's), so no separate handling.
    for (uint8_t z = 0; z < ZONE_COUNT; ++z) { pinMode(ZONES[z].ledPin, OUTPUT); digitalWrite(ZONES[z].ledPin, LOW); }

    // Watchdog handshake pins (§9 / #5.2). begin() parks the heartbeat LOW (no run,
    // no beat); the trip line is input-only with an external pull-up — idles HIGH,
    // the ATtiny drives it LOW (open-drain) to assert tripped.
    watchdog.begin(millis());
    pinMode(WD_TRIPPED_IN, INPUT);

#ifdef TINKLE_SIM
    // Sim flow source (#62): the firmware generates its own fake hall pulses —
    // 15 Hz square wave on a free pin (LEDC), looped back to FLOW_PIN in the
    // Wokwi diagram through the "FLOW on/off" slide switch (stock parts; no
    // custom chip, which the VS Code Wokwi runtime can't compile). 15 p/s at
    // the 450 K seed reads ~2.0 GPM. The switch BOOTS QUIET (common to GND):
    // a flow-on boot would cross the 50-pulse idle threshold in ~3.3 s and
    // latch FAULT_UNEXPECTED_FLOW before anyone touched a button. Slide it up
    // as a run starts; down mid-run for E1; up while idle for E2.
    constexpr uint8_t FLOW_SIM_PIN = 19;   // free per pins.h; sim-only loopback
    ledcSetup(0, 15, 10);                  // ch 0, 15 Hz, 10-bit
    ledcAttachPin(FLOW_SIM_PIN, 0);
    ledcWrite(0, 512);                     // 50% duty
#endif

    // Web layer last — everything it exposes exists by now. WiFi join is kicked
    // off here and watched from the loop (non-blocking); the server is live in
    // either mode and a phone finds it at http://tinkle.local.
    wifiManager.begin(millis());
    webServer.begin();

    Serial.println(F("[tinkle] boot: safe state, IDLE — cooperative loop running."));
}

void loop() {
    // One millis() read per pass; long actions time against it inside the modules.
    const uint32_t now = millis();
    const uint32_t t0  = micros();

    // Serialize this pass against the async HTTP handlers (web_server.h note): they
    // run on the other core and call into the same modules. Sub-ms hold, released
    // every pass, so a handler never waits longer than a tick or two.
    webServer.lock();

    wifiManager.tick(now);     // STA watcher / SoftAP fallback (§10). Inside the lock:
                               // it reads creds postSettings mutates, and /api/status
                               // reads the mode this mutates — both need the bracket.

    runController.tick(now);   // sole actuator commander; ticks the ValveDriver too
    buttons.tick(now);         // debounce + edge detect (§11)
    wallClock.tick(now);       // poll NTP / free-run the wall clock (§13)
    scheduler.tick(now);       // per-minute eval of due runs -> RunController (§13)
    flowMonitor.tick(flowSensor.pulses(), now);   // pulses -> gallons + rolling GPM (§7)

    // §11 manual buttons (DEC-006). RunController owns the single-active invariant; we
    // only map debounced edges onto it. One uniform policy for every zone button:
    //   FAULT      -> short press is a no-op (only the long-press clears);
    //   any run on -> any press STOPS it (no switching, no auto-start);
    //   IDLE       -> press starts that button's own zone.
    // A >=3 s long-press of any button clears a latched fault. A long hold fires
    // pressEdge first, but in FAULT that press is a no-op, so it harmlessly precedes
    // the clear 3 s later. requestClear() refuses unless actually faulted AND the
    // underlying condition has resolved (§14), so the ack only fires on a real clear.
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
        if (buttons.longPressEdge(z) && faultManager.requestClear())
            faultAckUntilMs = now + FAULT_ACK_MS;         // §12 success ack
    }

    // Per-run flow tally (§7): re-baseline gallons when a run starts pumping, and log the
    // measured volume when it stops. Edge-detected off the RUNNING state. (#35 adds the
    // no-flow / unexpected-flow faults on top of the same rate + accumulation.)
    static RunState prevRunState = RunState::Idle;
    const RunState runState = runController.state();
    if (runState == RunState::Running && prevRunState != RunState::Running)
        flowMonitor.resetAccumulation(flowSensor.pulses(), now);
    else if (runState != RunState::Running && prevRunState == RunState::Running)
        Serial.printf("[tinkle] run flow: %.2f gal @ %.2f GPM\n",
                      flowMonitor.gallons(), flowMonitor.rateGPM());
    prevRunState = runState;

    // Flow faults (§7/§14 / #35): no-flow during RUNNING (post-grace) / unexpected flow
    // during IDLE. The detector reads the post-tick rate + cumulative count against the
    // run state and returns the fault to raise; raiseFault is the sole actuator commander
    // and no-ops once latched, so calling it every tick is safe. The DEC-015 override
    // mutes inside the detector (setMuted — boot + /api/settings keep it current), so a
    // muted detector returns None here while measurement keeps flowing regardless.
    const Fault flowVerdict = flowFault.update(runState, flowMonitor.rateGPM(),
                                               flowSensor.pulses(), now);
    if (flowVerdict != Fault::None) runController.raiseFault(flowVerdict, now);

    // Calibration lifecycle (§7 / #36): tracks the cal run against the state machine,
    // freezing its pulse tally when the run settles. Keeps its own baseline, so the
    // RUNNING-edge re-baseline above can't disturb a calibration in progress.
    calibration.tick(runState, flowSensor.pulses(), now);

    // Watchdog handshake (§9 / #5.2): emit/park the heartbeat for this state and
    // turn the trip line into a verdict. Active-low line (pins.h): pulled HIGH at
    // rest, the ATtiny drives LOW to assert. TINKLE_SIM has no ATtiny and Wokwi
    // floats the unconnected input low, which would read "tripped" — force clear.
#ifdef TINKLE_SIM
    const bool wdTrip = false;
#else
    const bool wdTrip = digitalRead(WD_TRIPPED_IN) == LOW;
#endif
    runController.setWatchdogTripped(wdTrip);             // §4 pre-open gate
    // Re-read the state: a flow fault above may have latched THIS pass, and the
    // heartbeat must not outlive the pump command by even one tick.
    const Fault wdVerdict = watchdog.tick(runController.state(), wdTrip, now);
    if (wdVerdict != Fault::None) runController.raiseFault(wdVerdict, now);

    // Fault surface (§14 / #5.3): push each detector's CURRENT condition truth,
    // then let the manager log any newly latched fault. Watchdog resolved = trip
    // line released. Unexpected-flow resolved = the rolling rate decayed to 0 —
    // deliberately UNQUALIFIED by run state: the condition is only consulted while
    // that fault is latched (safe state, nothing should flow), and qualifying it
    // with isIdle() would read "resolved" the moment the fault latched (state =
    // Fault, not Idle), letting a clear through while water still moves.
    // NoFlow/CalRange are one-shot events with no live condition — they clear freely.
    faultManager.setConditionActive(Fault::Watchdog, wdTrip);
    // With the DEC-015 override on, the operator has declared the sensor untrust-
    // worthy — its pulses no longer block a clear (the API's clear-on-enable relies
    // on this staying false while overridden).
    faultManager.setConditionActive(Fault::UnexpectedFlow,
                                    !persistence.flowOverride() && flowMonitor.rateGPM() > 0.0f);
    faultManager.tick(now);

    // Auto-return self-test (DEC-014 / #52): fresh state read — a fault latching
    // this pass must abort the rest window, not get measured by it.
    const int restFlagged = valveRest.tick(runController.state(),
                                           runController.lastRun().zone,
                                           flowSensor.pulses(), now);
    if (restFlagged >= 0) {
        faultManager.note(Fault::ValveRest, now);
        Serial.printf("[tinkle] DEC-014 flag: zone %d still passing flow after close"
                      " — auto-return cap suspect, service the valve\n", restFlagged);
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

    // (No diverter-position persistence in v1.4: the two-leg NO/NC diverter rests plain
    // by construction, so there is no hold-state to cache — RunController sets the legs
    // per-run in PREP_DIVERTER, DEC-013.)

    // Core-state section over — let any waiting HTTP handler in. The async server
    // needs no tick; WiFi is watched at the top of the pass.
    webServer.unlock();

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
