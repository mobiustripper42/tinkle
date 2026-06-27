#include <Arduino.h>
#include "pins.h"
#include "arduino_gpio.h"
#include "../core/valve_driver.h"
#include "../core/run_controller.h"
#include "../core/loop_monitor.h"
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
// ValveDriver in turn). v1.5 (DEC-019) is phone-only: there is no button panel or
// TM1637 display — all interaction is the SPA over the web layer (§10.1), and the only
// on-board indicator is a ~1 Hz heartbeat LED. The scheduler / flow / web / watchdog
// modules hang off this same loop — each is one more non-blocking tick().

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
// shim (the only SNTP/timezone code). valid() feeds the SPA status clock (§10.1); until
// NTP lands (no WiFi before Phase 4) it stays false and the SPA shows the clock as unsynced.
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

// §2 tick budget. micros() resolution so sub-millisecond ticks still register.
static constexpr uint32_t TICK_BUDGET_US     = 10000;   // 10 ms
static constexpr uint32_t REPORT_INTERVAL_MS = 5000;
static LoopMonitor        loopMon(TICK_BUDGET_US);
static uint32_t           lastReportMs = 0;

// RunLog NVS write debounce (DEC-018): coalesce a queue-drain's rapid pushes into one
// ~352 B blob write. Runs are operator-rate, so this barely touches flash write-wear.
static constexpr uint32_t RUNLOG_PERSIST_DEBOUNCE_MS = 2000;

// Per-run volume as hundredths of a gallon for the RunLog record (DEC-018) — integer,
// clamped to the u16 field. Per-run gallons is always >= 0.
static uint16_t centigallonsOf(float gallons) {
    if (gallons <= 0.0f) return 0;
    const float cg = gallons * 100.0f + 0.5f;          // round to nearest hundredth
    return cg >= 65535.0f ? (uint16_t)65535u : (uint16_t)cg;
}

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

    // Rehydrate the run-history ring from its NVS blob (DEC-018) before the loop can push a
    // new entry. Absent blob => empty ring; the head feeds /api/status lastRun immediately.
    persistence.loadRunLog(runController.runLogRef());
    if (runController.runLog().count())
        Serial.printf("[tinkle] runlog: %u entries loaded\n", runController.runLog().count());

    // Rehydrate the fault-log ring from its NVS blob (#90) — the §8 fault log now survives
    // reboot. Absent blob => empty ring; feeds /api/status + /api/history immediately.
    persistence.loadFaultLog(faultManager);
    if (faultManager.logCount())
        Serial.printf("[tinkle] faultlog: %u entries loaded\n", faultManager.logCount());

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

    // Heartbeat "alive" LED (DEC-019): the only on-board indicator now that the TM1637
    // panel and button rings are gone. Off at boot; the loop blinks it at ~1 Hz so a
    // glance confirms the firmware is still ticking. It gates nothing — status proper
    // is the SPA (§10.1).
    pinMode(ALIVE_LED, OUTPUT);
    digitalWrite(ALIVE_LED, LOW);

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
    // the 1670 K seed reads ~0.54 GPM. The switch BOOTS QUIET (common to GND):
    // a flow-on boot would cross the 50-pulse idle threshold in ~3.3 s and
    // latch FAULT_UNEXPECTED_FLOW unprompted. Slide it up as a run starts (the
    // phone via the SPA); leave it up while idle to provoke E2 in the headless
    // sim. (Phone-only since DEC-019 — no buttons drive the run.)
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
    wallClock.tick(now);       // poll NTP / free-run the wall clock (§13)
    scheduler.tick(now);       // per-minute eval of due runs -> RunController (§13)
    flowMonitor.tick(flowSensor.pulses(), now);   // pulses -> gallons + rolling GPM (§7)

    // Manual run / stop / fault-clear are phone-only now (DEC-019): the SPA drives them
    // through the web layer (§10 — POST /api/run, /api/stop, /api/fault/clear), each
    // routed into the same RunController / FaultManager seams the buttons used to hit.
    // No on-box input path remains; the AC master switch is the physical kill (fail-dry).

    // Per-run flow tally (§7): re-baseline gallons when a run starts pumping, and log the
    // measured volume when it stops. Edge-detected off the RUNNING state. (#35 adds the
    // no-flow / unexpected-flow faults on top of the same rate + accumulation.)
    static RunState prevRunState = RunState::Idle;
    const RunState runState = runController.state();
    if (runState == RunState::Running && prevRunState != RunState::Running) {
        flowMonitor.resetAccumulation(flowSensor.pulses(), now);
        // Stamp the run's start time onto the pending RunLog entry (DEC-018). epoch()/valid()
        // read 0/false until NTP locks — RunController then flags the entry relative-to-uptime.
        runController.noteRunStart(wallClock.epoch(now), wallClock.valid());
    } else if (runState != RunState::Running && prevRunState == RunState::Running) {
        Serial.printf("[tinkle] run flow: %.2f gal @ %.2f GPM\n",
                      flowMonitor.gallons(), flowMonitor.rateGPM());
        // Capture the FINAL tally on the falling edge so a completed run's persisted volume
        // matches the printed gallons. SETTLE (a few ticks later, pump already off) consumes
        // this; the in-RUNNING push below alone would freeze it one tick early (DEC-018).
        runController.noteRunVolume(centigallonsOf(flowMonitor.gallons()));
    }
    // Keep the pending per-run volume fresh while pumping, so the entry pushed at SETTLE — or
    // on a fault mid-run — carries the measured centigallons, not a stale value (DEC-018).
    if (runState == RunState::Running)
        runController.noteRunVolume(centigallonsOf(flowMonitor.gallons()));
    prevRunState = runState;

    // Persist the run-history ring on change, debounced (DEC-018). RunController marks the log
    // dirty on each push (SETTLE / fault); write the blob once the ring has been quiet for the
    // debounce window, then clear the flag. Runs are minute-scale, so in practice this just
    // keeps the write off the hot loop and out of every tick — flash wear is a non-issue.
    static bool     runLogPending   = false;
    static uint32_t runLogPendingMs = 0;
    if (runController.runLogDirty()) {
        if (!runLogPending) { runLogPending = true; runLogPendingMs = now; }
        if ((uint32_t)(now - runLogPendingMs) >= RUNLOG_PERSIST_DEBOUNCE_MS) {
            persistence.saveRunLog(runController.runLog());
            runController.markRunLogPersisted();
            runLogPending = false;
        }
    } else {
        runLogPending = false;
    }

    // Persist the fault log IMMEDIATELY on change (#90) — deliberately NOT debounced like the
    // run log. A fault log exists to survive the resets it's most correlated with (brownout,
    // watchdog trip, crash), so a debounce window would swallow exactly the entry you want to
    // keep. Faults are rare (not loop-rate), so the per-fault ~96 B blob write is negligible
    // flash wear; and a latched fault has already commanded the safe state, so a few-ms blocking
    // write off the time-critical path is fine. The run log's coalescing rationale (minute-rate
    // runs, telemetry-grade loss) does not transfer here.
    if (faultManager.dirty()) {
        persistence.saveFaultLog(faultManager);
        faultManager.markPersisted();
    }

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
    // Wall-clock stamp for any fault logged this pass (#90 — pushed in like the runlog so the
    // fault log persists with a meaningful timestamp; epoch()/valid() are 0/false until NTP).
    const uint32_t faultEpoch = wallClock.epoch(now);
    const bool     faultClockValid = wallClock.valid();
    faultManager.tick(faultEpoch, faultClockValid);

    // Auto-return self-test (DEC-014 / #52): fresh state read — a fault latching
    // this pass must abort the rest window, not get measured by it.
    const int restFlagged = valveRest.tick(runController.state(),
                                           runController.lastRun().zoneIndex,
                                           flowSensor.pulses(), now);
    if (restFlagged >= 0) {
        faultManager.note(Fault::ValveRest, faultEpoch, faultClockValid);
        Serial.printf("[tinkle] DEC-014 flag: zone %d still passing flow after close"
                      " — auto-return cap suspect, service the valve\n", restFlagged);
    }

    // Heartbeat "alive" LED (DEC-019): ~1 Hz square wave = the firmware is still ticking.
    // Cosmetic only (gates nothing); the run state / fault / countdown that the TM1637
    // used to show now live in the SPA status screen (§10.1). digitalWrite is cheap, so
    // drive it every loop. One stretched blink at the ~49.7-day millis() wrap is harmless.
    digitalWrite(ALIVE_LED, (now / 500u) % 2u == 0u ? HIGH : LOW);

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
