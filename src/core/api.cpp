#include "api.h"

namespace tinkle {

const char* Api::stateName(RunState s) {
    switch (s) {
        case RunState::Idle:         return "idle";
        case RunState::PrepDiverter: return "prepDiverter";
        case RunState::OpenZone:     return "openZone";
        case RunState::StartPump:    return "startPump";
        case RunState::Running:      return "running";
        case RunState::StopPump:     return "stopPump";
        case RunState::CloseZone:    return "closeZone";
        case RunState::Settle:       return "settle";
        case RunState::Fault:        return "fault";
    }
    return "?";
}

const char* Api::faultName(Fault f) {
    switch (f) {
        case Fault::None:           return "none";
        case Fault::NoFlow:         return "noFlow";
        case Fault::UnexpectedFlow: return "unexpectedFlow";
        case Fault::Watchdog:       return "watchdog";
        case Fault::CalRange:       return "calRange";
        case Fault::Clock:          return "clock";
        case Fault::ValveRest:      return "valveRest";
        case Fault::MissedCycle:    return "missedCycle";
        default:                    return "?";
    }
}

const char* Api::resultName(RunResult r) {
    switch (r) {
        case RunResult::None:      return "none";
        case RunResult::Completed: return "completed";
        case RunResult::Stopped:   return "stopped";
        case RunResult::Faulted:   return "faulted";
    }
    return "?";
}

int Api::err(JsonDocument& out, int code, const char* msg) {
    out["error"] = msg;
    return code;
}

// ---------------------------------------------------------------------------- GET

int Api::getStatus(JsonDocument& out, uint32_t nowMs) {
    out["state"]        = stateName(d_.run.state());
    out["activeZone"]   = d_.run.activeZone();
    out["activeFert"]   = d_.run.activeFertigate();   // lastRun lags until run end
    out["remainingSec"] = d_.run.remainingSec(nowMs);
    out["queueDepth"]   = d_.run.queueDepth();

    JsonObject flow = out["flow"].to<JsonObject>();
    flow["gpm"]            = d_.flow.rateGPM();
    flow["gallons"]        = d_.flow.gallons();
    flow["k"]              = d_.flow.k();
    flow["overrideActive"] = d_.store.flowOverride();      // DEC-015 status flag

    const RunEntry& lr = d_.run.lastRun();              // the run-history ring head (DEC-018)
    JsonObject last = out["lastRun"].to<JsonObject>();
    last["result"]      = resultName(lr.result);
    last["zone"]        = lr.zoneIndex;
    last["durationSec"] = lr.durationSec;
    last["fertigate"]   = lr.fertigate;
    last["fault"]       = faultName((Fault)lr.faultCode);

    JsonObject fault = out["fault"].to<JsonObject>();
    fault["active"]       = faultName(d_.run.activeFault());
    fault["clearAllowed"] = d_.faults.clearAllowed();
    JsonArray log = fault["log"].to<JsonArray>();
    for (uint8_t i = 0; i < d_.faults.logCount(); ++i) {
        const FaultManager::Entry e = d_.faults.logEntry(i);
        JsonObject o = log.add<JsonObject>();
        o["code"]          = faultName(e.code);
        o["epoch"]         = e.epoch;            // wall-clock when clockWasValid, else !valid (#90)
        o["clockWasValid"] = e.clockWasValid;
    }

    out["valveRestFlags"] = d_.rest.flaggedMask();          // DEC-014 maintenance flags

    JsonObject clk = out["clock"].to<JsonObject>();
    clk["valid"] = d_.clock.valid();
    const WallTime wt = d_.clock.wall(nowMs);
    clk["hour"]    = wt.hour;
    clk["minute"]  = wt.minute;
    clk["weekday"] = wt.weekday;

    JsonObject cal = out["cal"].to<JsonObject>();
    switch (d_.cal.phase()) {
        case CalibrationController::Phase::Idle:     cal["phase"] = "idle";     break;
        case CalibrationController::Phase::Running:  cal["phase"] = "running";  break;
        case CalibrationController::Phase::Awaiting: cal["phase"] = "awaiting"; break;
    }
    cal["pulses"] = d_.cal.pulsesCounted();
    cal["lastK"]  = d_.cal.lastK();

    // Distributed-Watering day summary (#161) — the Home at-a-glance card. Present only when
    // Distributed mode is active AND the clock is valid (the summary is keyed to today's local
    // day/minute; a summary with no wall clock would be meaningless). Every field is exact:
    // completed-cycle counts + metered centigallons, no estimate.
    if (d_.clock.valid()) {
        const DistributedPlan plan = computeDistributedPlan(d_.sched.distributed(), d_.sched.zoneCount());
        const uint16_t nowMin = (uint16_t)(wt.hour * 60 + wt.minute);
        const uint32_t dayOrd = d_.clock.epoch(nowMs) / 86400u;
        const DistDaySummary ds = computeDistSummary(d_.run.runLog(), plan, d_.sched.zoneCount(),
                                                     dayOrd, nowMin);
        if (ds.active) {
            JsonObject dsum = out["distSummary"].to<JsonObject>();
            dsum["cycles"] = ds.cycles;
            JsonArray zones = dsum["zones"].to<JsonArray>();
            for (uint8_t z = 0; z < ds.zoneCount; ++z) {
                JsonObject o = zones.add<JsonObject>();
                o["plannedByNow"] = ds.zones[z].plannedByNow;
                o["completed"]    = ds.zones[z].completed;
                o["centigallons"] = ds.zones[z].centigallons;
            }
        }
    }

    out["scheduleDropped"] = d_.sched.dropped();
    return 200;
}

int Api::getHistory(JsonDocument& out, uint32_t nowMs) {
    // Read-only telemetry (DEC-018): the run-history ring + the fault ring, both newest-first,
    // plus render flags. No FAULT gate, no validation — §10 gating is mutating-paths-only. Lazy-
    // fetched when the History screen opens, kept off the 1–2 s /api/status poll hot path.
    const RunLog& rl = d_.run.runLog();
    JsonArray runs = out["runs"].to<JsonArray>();
    for (uint8_t i = 0; i < rl.count(); ++i) {              // at(0) = newest run
        const RunEntry e = rl.at(i);
        JsonObject o = runs.add<JsonObject>();
        o["startEpoch"]    = e.startEpoch;                  // wall-clock only when clockWasValid
        o["clockWasValid"] = e.clockWasValid;
        o["zone"]          = e.zoneIndex;
        o["durationSec"]   = e.durationSec;
        o["gallons"]       = e.centigallons / 100.0f;       // u16 hundredths -> gallons
        o["fertigate"]     = e.fertigate;
        o["result"]        = resultName(e.result);
        o["fault"]         = faultName((Fault)e.faultCode);
    }

    // Fault ring — now epoch-stamped + NVS-persisted (#90, resolving #72's RAM-only gap). Same
    // {code, epoch, clockWasValid} shape as /api/status + the run rows, so the SPA reuses one
    // wall-clock-or-"unsynced" renderer across all three.
    JsonArray faults = out["faults"].to<JsonArray>();
    for (uint8_t i = 0; i < d_.faults.logCount(); ++i) {
        const FaultManager::Entry fe = d_.faults.logEntry(i);
        JsonObject o = faults.add<JsonObject>();
        o["code"]          = faultName(fe.code);
        o["epoch"]         = fe.epoch;
        o["clockWasValid"] = fe.clockWasValid;
    }

    out["clockValid"] = d_.clock.valid();                  // current clock state (render flag)
    out["uptimeMs"]   = nowMs;                             // device uptime telemetry (parallels /api/status)
    return 200;
}

int Api::getSchedule(JsonDocument& out) {
    JsonArray arr = out["entries"].to<JsonArray>();
    for (uint8_t i = 0; i < d_.sched.count(); ++i) {
        const ScheduleEntry& e = d_.sched.entry(i);
        JsonObject o = arr.add<JsonObject>();
        o["id"]           = e.id;
        o["zoneIndex"]    = e.zoneIndex;
        o["hour"]         = e.hour;
        o["minute"]       = e.minute;
        o["durationSec"]  = e.durationSec;
        o["daysMask"]     = e.daysMask;
        o["fertOverride"] = (uint8_t)e.fertOverride;
        o["enabled"]      = e.enabled;
    }
    out["max"]     = (uint8_t)Scheduler::MAX_ENTRIES;   // rvalue: C++11 odr-use guard
    out["dropped"] = d_.sched.dropped();
    return 200;
}

int Api::getSettings(JsonDocument& out) {
    JsonArray zd = out["zoneDefaults"].to<JsonArray>();
    for (uint8_t z = 0; z < d_.store.zoneCount(); ++z) zd.add(d_.store.zoneDefaultSec(z));
    out["swMaxRuntimeSec"] = d_.store.swMaxRuntimeSec();
    out["pulsesPerGallon"] = d_.store.pulsesPerGallon();
    out["flowOverride"]    = d_.store.flowOverride();
    JsonObject wifi = out["wifi"].to<JsonObject>();
    wifi["ssid"]       = d_.store.wifiSsid();
    wifi["configured"] = d_.store.wifiSsid()[0] != '\0';
    // Never echo the passphrase: write-only by design.
    return 200;
}

int Api::getDistributed(JsonDocument& out) {
    const DistributedConfig c = d_.sched.distributed();
    out["enabled"]        = c.enabled;
    out["windowStartMin"] = c.windowStartMin;
    out["windowEndMin"]   = c.windowEndMin;
    out["perZoneMin"]     = c.perZoneMin;
    out["fertCount"]      = c.fertCount;
    out["active"]         = d_.sched.distributedActive();   // enabled AND schedulable
    // Envelope + zone count so the SPA preview mirrors computeDistributedPlan exactly.
    out["floorMin"]    = (uint16_t)DIST_RUN_FLOOR_MIN;
    out["maxRuns"]     = (uint8_t)DIST_MAX_RUNS;
    out["overheadSec"] = (uint16_t)DIST_RUN_OVERHEAD_SEC;
    out["zoneCount"]   = d_.sched.zoneCount();
    // The authoritative server-computed plan (what the controller WILL fire) — the SPA shows
    // this after a save and mirrors it client-side while editing.
    const DistributedPlan p = computeDistributedPlan(c, d_.sched.zoneCount());
    if (p.valid) {
        JsonObject po = out["plan"].to<JsonObject>();
        po["cycles"]    = p.cycles;
        po["runLenSec"] = p.runLenSec;
        JsonArray cs = po["cycleStartMin"].to<JsonArray>();
        for (uint8_t i = 0; i < p.cycles; ++i) cs.add(p.cycleStartMin[i]);
    }
    return 200;
}

// --------------------------------------------------------------------------- POST

int Api::postRun(JsonVariantConst in, JsonDocument& out, uint32_t nowMs) {
    if (d_.run.isFaulted()) return err(out, 409, "faulted");
    if (!in["zoneIndex"].is<int>()) return err(out, 400, "zoneIndex required");
    const int zone = in["zoneIndex"].as<int>();
    if (zone < 0 || zone >= (int)d_.store.zoneCount()) return err(out, 422, "zoneIndex out of range");

    uint32_t dur = d_.store.zoneDefaultSec((uint8_t)zone);
    if (!in["durationSec"].isNull()) {
        if (!in["durationSec"].is<int>()) return err(out, 400, "durationSec must be a number");
        const int reqDur = in["durationSec"].as<int>();
        if (reqDur < (int)RUN_MIN_SEC || reqDur > (int)RUN_MAX_SEC)
            return err(out, 422, "durationSec out of range");
        dur = (uint32_t)reqDur;
    }

    RunRequest req;
    req.zoneIndex   = (uint8_t)zone;
    req.durationSec = dur;
    req.fertigate   = in["fertigate"].as<bool>();          // absent -> false

    const bool wasIdle = d_.run.isIdle();
    if (!d_.run.requestRun(req, nowMs)) return err(out, 409, "refused (queue full?)");
    out["accepted"]    = true;
    out["zoneIndex"]   = req.zoneIndex;
    out["durationSec"] = req.durationSec;
    out["fertigate"]   = req.fertigate;
    out["queued"]      = !wasIdle;
    return 200;
}

int Api::postStop(JsonDocument& out, uint32_t nowMs) {
    // §10: cancel all, unwind to safe state. Allowed in FAULT (it's a no-op there —
    // the safe state is already commanded). A calibration in progress is voided, not
    // judged: the operator bailed, so no K and no CalRange.
    if (d_.cal.active()) d_.cal.cancel(nowMs);
    d_.run.stop(nowMs);
    out["stopped"] = true;
    return 200;
}

int Api::postSchedule(JsonVariantConst in, JsonDocument& out, uint32_t nowMs) {
    if (d_.run.isFaulted()) return err(out, 409, "faulted");
    JsonArrayConst arr = in["entries"].as<JsonArrayConst>();
    if (arr.isNull()) return err(out, 400, "entries required");
    if (arr.size() > Scheduler::MAX_ENTRIES) return err(out, 422, "too many entries");

    // Validate EVERYTHING before touching the live schedule — the replace is atomic:
    // a bad entry rejects the whole post and the running schedule stays untouched.
    ScheduleEntry staged[Scheduler::MAX_ENTRIES];
    uint8_t n = 0;
    for (JsonVariantConst v : arr) {
        if (!v["zoneIndex"].is<int>() || !v["hour"].is<int>() || !v["minute"].is<int>() ||
            !v["durationSec"].is<int>() || !v["daysMask"].is<int>())
            return err(out, 400, "entry missing required field");
        const int zone = v["zoneIndex"].as<int>();
        const int hour = v["hour"].as<int>();
        const int min  = v["minute"].as<int>();
        const int dur  = v["durationSec"].as<int>();
        const int days = v["daysMask"].as<int>();
        const int fert = v["fertOverride"].isNull() ? 0 : v["fertOverride"].as<int>();
        if (zone < 0 || zone >= (int)d_.store.zoneCount()) return err(out, 422, "zoneIndex out of range");
        if (hour < 0 || hour > 23 || min < 0 || min > 59)  return err(out, 422, "time out of range");
        if (dur < (int)RUN_MIN_SEC || dur > (int)RUN_MAX_SEC) return err(out, 422, "durationSec out of range");
        if (days < 1 || days > 0x7F)                       return err(out, 422, "daysMask out of range");
        if (fert < 0 || fert > 2)                          return err(out, 422, "fertOverride out of range");

        ScheduleEntry& e = staged[n];
        e.id           = n;                                // server-assigned, stable per save
        e.zoneIndex    = (uint8_t)zone;
        e.hour         = (uint8_t)hour;
        e.minute       = (uint8_t)min;
        e.durationSec  = (uint16_t)dur;
        e.daysMask     = (uint8_t)days;
        e.fertOverride = (FertOverride)fert;
        e.enabled      = v["enabled"].as<bool>();          // absent -> false
        ++n;
    }

    d_.sched.clear();
    for (uint8_t i = 0; i < n; ++i) d_.sched.add(staged[i]);
    d_.store.saveScheduleEntries(staged, n);               // §13 save-on-edit
    d_.sched.evalNow(nowMs);                               // §13 "checks ... on edit"

    out["saved"] = true;
    out["count"] = n;
    return 200;
}

int Api::postDistributed(JsonVariantConst in, JsonDocument& out, uint32_t nowMs) {
    if (d_.run.isFaulted()) return err(out, 409, "faulted");
    // Seed from the current record so a disable ({enabled:false}) just flips the mode off and
    // keeps the window/minutes/fert — toggling back on shouldn't force a full re-entry. An
    // enable overwrites those fields from the body below.
    DistributedConfig c = d_.sched.distributed();
    c.enabled = in["enabled"].as<bool>();                  // absent -> false (disable)

    if (c.enabled) {
        if (!in["windowStartMin"].is<int>() || !in["windowEndMin"].is<int>() ||
            !in["perZoneMin"].is<int>())
            return err(out, 400, "window and perZoneMin required");
        const int ws = in["windowStartMin"].as<int>();
        const int we = in["windowEndMin"].as<int>();
        const int pm = in["perZoneMin"].as<int>();
        const int fc = in["fertCount"].isNull() ? 0 : in["fertCount"].as<int>();
        if (ws < 0 || ws > 1439 || we < 1 || we > 1440)  return err(out, 422, "window out of range");
        if (pm < 0 || pm > (int)DIST_MAX_PERZONE_MIN)    return err(out, 422, "perZoneMin out of range");
        if (fc < 0 || fc > (int)DIST_MAX_RUNS)           return err(out, 422, "fertCount out of range");
        c.windowStartMin = (uint16_t)ws;
        c.windowEndMin   = (uint16_t)we;
        c.perZoneMin     = (uint16_t)pm;
        c.fertCount      = (uint8_t)fc;
        // Reject an enabled config that can't be scheduled (below floor / over-subscribed /
        // bad window). The SPA blocks this up front; this is the authoritative server backstop.
        if (!computeDistributedPlan(c, d_.sched.zoneCount()).valid)
            return err(out, 422, "not schedulable (check window, minutes, floor)");
    }

    d_.store.setDistributed(c);       // persist (atomic; own NVS key)
    d_.sched.setDistributed(c);       // live — either/or with the entry schedule
    d_.sched.evalNow(nowMs);          // §13 checks-on-edit
    out["saved"]   = true;
    out["enabled"] = c.enabled;
    out["active"]  = d_.sched.distributedActive();
    return 200;
}

int Api::postSettings(JsonVariantConst in, JsonDocument& out, uint32_t nowMs) {
    (void)nowMs;
    // Allowed in FAULT (header note): settings command no actuation, and DEC-015's
    // enable-while-latched is the recovery path for a lying flow sensor.
    if (in.isNull()) return err(out, 400, "body required");

    // Validate the whole payload before applying any of it (atomic, like /schedule).
    if (!in["swMaxRuntimeSec"].isNull()) {
        if (!in["swMaxRuntimeSec"].is<int>()) return err(out, 400, "swMaxRuntimeSec must be a number");
        const int v = in["swMaxRuntimeSec"].as<int>();
        if (v < (int)SW_MAX_MIN || v > (int)SW_MAX_MAX) return err(out, 422, "swMaxRuntimeSec out of range");
    }
    if (!in["pulsesPerGallon"].isNull()) {
        if (!in["pulsesPerGallon"].is<float>()) return err(out, 400, "pulsesPerGallon must be a number");
        const float k = in["pulsesPerGallon"].as<float>();
        if (k < K_MIN || k > K_MAX) return err(out, 422, "pulsesPerGallon out of range");
    }
    JsonArrayConst zd = in["zoneDefaults"].as<JsonArrayConst>();
    if (!in["zoneDefaults"].isNull()) {
        if (zd.isNull()) return err(out, 400, "zoneDefaults must be an array");
        if (zd.size() > d_.store.zoneCount()) return err(out, 422, "too many zoneDefaults");
        for (JsonVariantConst v : zd) {
            if (!v.is<int>()) return err(out, 400, "zoneDefaults must be numbers");
            const int s = v.as<int>();
            if (s < (int)RUN_MIN_SEC || s > (int)RUN_MAX_SEC)
                return err(out, 422, "zoneDefault out of range");
        }
    }
    JsonVariantConst wifi = in["wifi"];
    if (!wifi.isNull()) {
        if (!wifi["ssid"].is<const char*>()) return err(out, 400, "wifi.ssid required");
        // A non-string pass would coerce to nullptr and silently store an EMPTY
        // passphrase — wrong network at next boot. Reject loudly instead.
        if (!wifi["pass"].isNull() && !wifi["pass"].is<const char*>())
            return err(out, 400, "wifi.pass must be a string");
    }
    // The DEC-015 safety knob is the worst field to be lenient on: a present-but-
    // non-bool value (e.g. 1) must 400 like every other field, not silently drop.
    if (!in["flowOverride"].isNull() && !in["flowOverride"].is<bool>())
        return err(out, 400, "flowOverride must be a boolean");

    // --- apply ---
    if (!in["swMaxRuntimeSec"].isNull()) {
        const uint32_t v = (uint32_t)in["swMaxRuntimeSec"].as<int>();
        d_.store.setSwMaxRuntimeSec(v);
        d_.run.setSwMaxRuntimeSec(v);                      // live: caps the current run too
    }
    if (!in["pulsesPerGallon"].isNull()) {
        const float k = in["pulsesPerGallon"].as<float>();
        d_.store.setPulsesPerGallon(k);
        d_.flow.setK(k);
    }
    if (!zd.isNull()) {
        uint8_t z = 0;
        for (JsonVariantConst v : zd) d_.store.setZoneDefaultSec(z++, (uint32_t)v.as<int>());
    }
    if (!wifi.isNull()) {
        // Stored now, joined at next boot (the WiFi manager reads creds at begin()).
        d_.store.setWifiCreds(wifi["ssid"].as<const char*>(),
                              wifi["pass"].isNull() ? "" : wifi["pass"].as<const char*>());
        out["wifiNote"] = "applies at next boot";
    }
    if (in["flowOverride"].is<bool>()) {
        const bool on = in["flowOverride"].as<bool>();
        d_.store.setFlowOverride(on);
        d_.flowFault.setMuted(on);                         // DEC-015: mute faults, not measurement
        if (on) {
            // DEC-015: enabling clears any latched FLOW fault — the operator has
            // declared the sensor untrustworthy, so its condition no longer blocks
            // the sanctioned clear path. Only flow faults; everything else keeps
            // its gate (the watchdog is untouchable from here by construction).
            d_.faults.setConditionActive(Fault::UnexpectedFlow, false);
            const Fault f = d_.run.activeFault();
            if (f == Fault::NoFlow || f == Fault::UnexpectedFlow) d_.faults.requestClear();
        }
    }

    return getSettings(out);                               // echo the applied state
}

int Api::postCalStart(JsonVariantConst in, JsonDocument& out, uint32_t pulses, uint32_t nowMs) {
    if (d_.run.isFaulted()) return err(out, 409, "faulted");
    if (!in["zoneIndex"].is<int>()) return err(out, 400, "zoneIndex required");
    const int zone = in["zoneIndex"].as<int>();
    if (zone < 0 || zone >= (int)d_.store.zoneCount()) return err(out, 422, "zoneIndex out of range");
    if (!d_.cal.start((uint8_t)zone, pulses, nowMs))
        return err(out, 409, "refused (busy or already calibrating)");
    out["started"]   = true;
    out["zoneIndex"] = zone;
    return 200;
}

int Api::postCalFinish(JsonVariantConst in, JsonDocument& out, uint32_t pulses, uint32_t nowMs) {
    if (d_.run.isFaulted()) return err(out, 409, "faulted");
    if (!in["measuredGallons"].is<float>()) return err(out, 400, "measuredGallons required");
    const float gal = in["measuredGallons"].as<float>();
    // Parse-sane bounds only — the controller's own §15 sanity bounds are the real
    // judge, and a failing measurement latches FAULT_CAL_RANGE by design (§7).
    if (!(gal >= GAL_MIN && gal <= GAL_MAX)) return err(out, 422, "measuredGallons out of range");

    switch (d_.cal.finish(gal, pulses, nowMs)) {
        case CalibrationController::FinishResult::Ok:
            out["k"]      = d_.cal.lastK();
            out["pulses"] = d_.cal.pulsesCounted();
            return 200;
        case CalibrationController::FinishResult::NotCalibrating:
            return err(out, 409, "no calibration in progress");
        case CalibrationController::FinishResult::Rejected:
        default:
            return err(out, 422, "rejected — FAULT_CAL_RANGE latched");
    }
}

int Api::postFaultClear(JsonDocument& out, uint32_t nowMs) {
    (void)nowMs;
    const Fault before = d_.run.activeFault();
    if (d_.faults.requestClear()) {
        out["cleared"] = true;
        out["was"]     = faultName(before);
        return 200;
    }
    out["cleared"]      = false;
    out["clearAllowed"] = d_.faults.clearAllowed();
    out["fault"]        = faultName(d_.run.activeFault());
    return 409;
}

int Api::postOtaBegin(JsonDocument& out, uint32_t nowMs) {
    (void)nowMs;
    if (d_.run.otaActive()) return err(out, 409, "OTA already in progress");
    // IDLE or FAULT only — every transitional state may have the pump or a valve
    // energized. queueDepth() is belt-and-braces: chained runs skip IDLE, but a
    // future state-machine change must not silently open this gate.
    const bool safe = (d_.run.isIdle() || d_.run.isFaulted()) && d_.run.queueDepth() == 0;
    if (!safe) return err(out, 409, "run active or queued — OTA refused");
    d_.run.setOtaActive(true);
    out["otaReady"] = true;
    return 200;
}

void Api::otaAbort() {
    d_.run.setOtaActive(false);
}

} // namespace tinkle
