#pragma once
#include <stdint.h>
#include <ArduinoJson.h>
#include "run_controller.h"
#include "scheduler.h"
#include "persistence.h"
#include "fault_manager.h"
#include "calibration_controller.h"
#include "flow_monitor.h"
#include "flow_fault_detector.h"
#include "valve_rest_monitor.h"
#include "dist_summary.h"
#include "clock.h"

// Api — the §10 endpoint POLICY (#56), platform-independent on purpose. Everything
// the web layer decides — range validation, FAULT gating, JSON shapes, the DEC-015
// clear-on-enable — lives here so `pio test -e native` exercises it; the ESP32 glue
// (src/esp32/web_server.h) is just HTTP plumbing: route -> parsed body in ->
// JsonDocument out -> serialized response. ArduinoJson compiles on the host, so the
// tests assert on the real wire shapes, not a parallel model.
//
// CONTRACT (§10):
// - Every handler returns an HTTP status code and fills `out` (the response body).
//   200 OK · 400 malformed/missing field · 409 wrong state (faulted, busy, not
//   calibrating, clear refused) · 422 well-formed but out of range.
// - Mutating endpoints reject in FAULT — except /stop, /fault/clear, and /settings.
//   The /settings exception is deliberate (documented §10 deviation): settings
//   command no actuation, and DEC-015 *requires* enabling the flow override while
//   its flow fault is latched (that enable is the recovery path).
// - /fault/clear routes through FaultManager::requestClear() — the only sanctioned
//   clearFault() path; the resolved-condition gate applies to HTTP exactly as it
//   does to the button long-press.
// - Actuation only ever moves through RunController (sole commander) — this module
//   requests; it never touches ValveDriver.
//
// Time and pulses are passed in per call (the glue reads millis()/FlowSensor), so
// the policy stays clock-free and host-testable like every other core module.

namespace tinkle {

class Api {
public:
    struct Deps {
        RunController&         run;
        Scheduler&             sched;
        Persistence&           store;
        FaultManager&          faults;
        CalibrationController& cal;
        FlowMonitor&           flow;
        FlowFaultDetector&     flowFault;
        ValveRestMonitor&      rest;
        Clock&                 clock;
    };

    // §10 validation ranges — seeds, same philosophy as §15: bench/field-tune.
    static constexpr uint32_t RUN_MIN_SEC   = 10;     // shortest meaningful watering
    static constexpr uint32_t RUN_MAX_SEC   = 7200;   // sanity; swMaxRuntimeSec still caps
    static constexpr uint32_t SW_MAX_MIN    = 60;
    static constexpr uint32_t SW_MAX_MAX    = 1800;   // never above the ATtiny ceiling (§9)
    static constexpr float    K_MIN         = 50.0f;  // mirror the §15 cal sanity bounds
    static constexpr float    K_MAX         = 5000.0f;
    static constexpr float    GAL_MIN       = 0.01f;  // parse-sane; cal's own minGallons judges
    static constexpr float    GAL_MAX       = 100.0f;

    explicit Api(const Deps& d) : d_(d) {}

    // --- GET ---
    int getStatus  (JsonDocument& out, uint32_t nowMs);
    int getSchedule(JsonDocument& out);
    int getSettings(JsonDocument& out);
    int getHistory (JsonDocument& out, uint32_t nowMs);   // DEC-018 run + fault history (#70)
    int getDistributed(JsonDocument& out);                // DEC-024 Distributed Watering config + plan

    // --- POST (body pre-parsed by the glue; `in` may be a null variant for no body) ---
    int postRun      (JsonVariantConst in, JsonDocument& out, uint32_t nowMs);
    int postStop     (JsonDocument& out, uint32_t nowMs);
    int postSchedule (JsonVariantConst in, JsonDocument& out, uint32_t nowMs);
    int postSettings (JsonVariantConst in, JsonDocument& out, uint32_t nowMs);
    int postDistributed(JsonVariantConst in, JsonDocument& out, uint32_t nowMs);  // DEC-024
    int postCalStart (JsonVariantConst in, JsonDocument& out, uint32_t pulses, uint32_t nowMs);
    int postCalFinish(JsonVariantConst in, JsonDocument& out, uint32_t pulses, uint32_t nowMs);
    int postFaultClear(JsonDocument& out, uint32_t nowMs);

    // OTA gate (#126). The upload mechanics (Update.h, chunk streaming, the secret
    // header) are ESP32 glue; the DECISION of when a reflash may begin is policy
    // and lives here. 200 = accepted, RunController's OTA inhibit is now set (no
    // run — scheduled or manual — can start until reboot or otaAbort()). 409 =
    // wrong state. Allowed from IDLE *or* FAULT: a latched fault is pump-off and
    // queue-unwound — the safest time to reflash, and OTA-while-faulted is a
    // legitimate recovery path (the latch is RAM-only and re-derives from live
    // conditions after reboot, so nothing is laundered past the clear gate).
    int  postOtaBegin(JsonDocument& out, uint32_t nowMs);
    // Failed/aborted upload: lift the inhibit (a successful flash ends in reboot).
    void otaAbort();

    // Wire-name helpers (status strings; also used by tests).
    static const char* stateName(RunState s);
    static const char* faultName(Fault f);
    static const char* resultName(RunResult r);   // run outcome (lastRun + history, DEC-018)

private:
    int err(JsonDocument& out, int code, const char* msg);

    Deps d_;
};

} // namespace tinkle
