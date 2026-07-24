# Tinkle — Firmware Specification, V1

**Target:** ESP32 DevKitC (38-pin), Arduino-ESP32 framework, PlatformIO project.
**Companion binary:** ATtiny85 watchdog (separate sketch, §9).
**Audience:** implementation handoff to Claude Code. This is the *what* and the *contracts*; CC owns the *how*.

This spec assumes the pin map and arming architecture in `tinkle_wiring.html`. Where they disagree, the wiring doc wins on pins, this doc wins on behavior.

---

## 1. Responsibilities

In scope for V1:
- Execute a stored watering schedule locally, with no network dependency.
- Sequence zones one at a time; drive the motorized valves (zones + diverter) and pump in the correct order.
- Per-run fertigation control via the diverter, defaulting to one fert run/day.
- Monitor flow as a sanity cross-check; fault on mismatch.
- Serve a local web config UI over Wi-Fi.
- Field-calibrate the flow sensor without reflashing.
- Blink a board-level alive LED (~1 Hz) as a liveness indicator. (v1.5 / DEC-019 is phone-only — the former TM1637 countdown display and three button LED rings are cut; all status now lives in the SPA, §10.1.)
- Maintain a cooperative heartbeat with the ATtiny watchdog and honor its trips.
- Fail dry on every fault and on power loss.

Explicitly **out of scope** for V1: MQTT/cloud, sensor-driven (closed-loop) irrigation, multi-tunnel logic (the board has headroom; the firmware targets 3 zones — Red's 2 + the Zone 3 hose outlet — plus 1 diverter). Keep the zone/tunnel model data-driven so adding zones later is config, not a rewrite.

---

## 2. Architecture

Single-core cooperative loop, **fully non-blocking** — no `delay()` anywhere in the run path. Target loop tick ≤ 10 ms. Long actions (valve travel, run durations) are timed against `millis()`, not blocked on.

Suggested modules (one translation unit each):

- `Clock` — NTP sync + free-running fallback; exposes wall-clock + monotonic ms.
- `Persistence` — NVS/Preferences read/write of all stored state (§8).
- `ValveDriver` — low-level motorized-valve travel (zones + diverter, one on/off FET each) + pump relay.
- `FlowMonitor` — ISR pulse count, rate calc, calibration, fault detection.
- `RunController` — the state machine (§4); owns the actuation sequence.
- `Scheduler` — evaluates schedule entries, enqueues due runs, assigns fert flag.
- `WebConfig` — async HTTP server + JSON API (§10).
- `Watchdog` — heartbeat emitter + reads ATtiny trip line; forces safe state on trip.
- `FaultManager` — fault state, latch, clear, log ring buffer.

`RunController` is the only module allowed to command `ValveDriver` actuators. Everything else requests runs through it. This keeps the actuation sequence in one auditable place.

---

## 3. Hardware interface (pin constants)

Mirror the wiring doc. Define once in `pins.h`:

Every valve is on/off — **one low-side FET per valve, one GPIO each** (no H-bridges, no
IN1/IN2 pairs, no master). All non-strapping outputs, so the DEC-007 strapping-pin
contortion is gone (DEC-011).

```
// Zone valves — NC, one low-side FET each (energize = open, de-energize = cap-return closed)
Z1_FET=13  Z2_FET=14  Z3_FET=16     // Z3 = hose outlet (build-for-three)
// Diverter legs — one low-side FET each
DIV_CLEAN_FET=17   // NO bypass leg  (de-energized = open  = plain water flows)
DIV_FERT_FET=18    // NC Dosatron leg (de-energized = closed)
// Actuator
PUMP_RELAY=22      // on the ARMED 24V (the fail-dry source gate, DEC-012)
// Sensing
FLOW_PIN=27            // interrupt, level-shifted to 3.3V
// Alive / board-health LED (DEC-019) — DevKitC onboard LED, ~1 Hz blink, gates nothing
ALIVE_LED=2
// Watchdog
HEARTBEAT_OUT=4        // ATtiny heartbeat handshake — NOT the alive LED above
WD_TRIPPED_IN=36
// Phone-only (DEC-019): the TM1637 display (was 25/26), the three zone buttons
// (was input-only 34/35/39), and the three button LED rings (was 23/32/33) are gone.
// Free for more zones (build-for-three): 21, 5, 12, 15 + the freed 23/25/26/32/33/34/35/39
// (and 19, used only by the TINKLE_SIM flow loopback)
```

Each FET gate gets a series resistor + a gate-to-GND pulldown so the valve sits **off**
(closed/rest) through ESP32 boot. Model zones as a table so the count is data-driven:

```
struct Zone { uint8_t fetPin; const char* name; };   // v1.5: no per-zone LED/button (DEC-019)
Zone zones[] = {
  {Z1_FET, "Zone 1"},   // Red Tunnel beds 1–3
  {Z2_FET, "Zone 2"},   // Red Tunnel beds 4–6
  {Z3_FET, "Zone 3"},   // general-purpose hose outlet (build-for-three)
};
```

---

## 4. Run state machine

States: `IDLE`, `PREP_DIVERTER`, `OPEN_ZONE`, `START_PUMP`, `RUNNING`, `STOP_PUMP`, `CLOSE_ZONE`, `SETTLE`, `FAULT`.

A run request carries: `{ zoneIndex, durationSec, fertigate }`. Source = scheduler or manual.

There is no master valve (DEC-012) — the pump on the armed 24V is the source gate, so the
sequence opens the zone, starts the pump, and stops it; no master open/close steps.

Sequence (each step non-blocking, advancing on its own timer/confirmation):

1. **PREP_DIVERTER** — set the two diverter legs for `fertigate` (fert → both leg FETs energized: fert-leg opens, bypass closes; plain → both de-energized: bypass rests open, fert-leg rests closed). Wait `DIVERTER_TRAVEL_MS` if anything changed; skip if already in the wanted state.
2. **OPEN_ZONE** — energize the zone FET (NC valve drives open); wait `ZONE_TRAVEL_MS`. The pump waits for this (the state machine gates on the valve no longer being busy), so the pump never loads against a closed/mid-travel valve. The watchdog pre-open gate sits at the PREP_DIVERTER→OPEN_ZONE transition (DEC-023): while the qualified trip line is asserted the run **holds** (nothing energized — the lockout self-releases ≤ 2 s after the previous run's heartbeat quiets); a line stuck asserted past `WD_WAIT_MS` skips this run only (logged Faulted) and the queue advances.
3. **START_PUMP** — close `PUMP_RELAY`. Begin `RUNNING`.
4. **RUNNING** — run for `durationSec`. Continuously: run flow checks (§7), watch software max-runtime, surface the live countdown to the SPA (§10.1). Any fault → jump to STOP_PUMP path then FAULT.
5. **STOP_PUMP** — open pump relay (this is what stops water — the source).
6. **CLOSE_ZONE** — de-energize the zone FET; the cap auto-return drives it closed over `ZONE_TRAVEL_MS`. (Pump is already off, so the valve closes against no pressure.)
7. **SETTLE** — brief dwell, push the run entry (zone, start epoch, duration, gallons, fert y/n, result) onto the `RunLog` ring (DEC-018) — the ring **head** is the "last run" the status API exposes. Diverter legs returned to plain rest when no run is queued (DEC-021); left as-is while a run is queued (the chained run sets them in its own PREP). → IDLE (or next queued run).

Only one run active at a time. Queued requests run sequentially with a short inter-run gap. A stop/cancel request unwinds straight to STOP_PUMP → CLOSE_ZONE from any active step.

---

## 5. Valve actuation contract (`ValveDriver`)

Every channel is a **single on/off output** (a low-side FET). The safe/rest level is **LOW
(FET off)** for all of them — NC zones closed, NC fert-leg closed, NO bypass-leg open
(plain), pump off. There are no H-bridges and **no never-both-high invariant**.

- `openZone(zone)` — set the zone FET HIGH (the NC valve drives open); start a `ZONE_TRAVEL_MS` busy timer.
- `closeZone(zone)` — set the zone FET LOW; the capacitor auto-return drives it closed over `ZONE_TRAVEL_MS` (busy until then).
- `setDiverter(fertigate)` — `fertigate` → set **both** leg FETs HIGH (fert-leg opens, bypass closes); `!fertigate` → set **both** LOW (rest: bypass open, fert-leg closed). Hold `DIVERTER_TRAVEL_MS` busy. No NVS cache needed — the rest state is defined by the NO/NC valve types, not by memory.
- `pumpOn()/pumpOff()` — set/clear `PUMP_RELAY`.
- On boot and on any FAULT entry: force pump off and **de-energize every valve FET** — zones cap-close, the diverter returns to plain. This is the **safe state**.

The travel timers must be independent per actuator so a diverter travel doesn't block a zone's travel. `zoneBusy(zone)` / `diverterBusy()` gate the §4 sequence.

> **Implemented (task 1.8):** `ValveDriver` and `pins.h` are the on/off model above — one
> low-side FET per valve (`openZone`/`closeZone`), no `MASTER_FET`, the diverter as two leg
> FETs, the `pins.h` re-map, and the native tests rewritten. The §4 sequence kept its
> `zoneBusy()`/`diverterBusy()` gates; only the master open/close states were dropped
> (DEC-011/012/013).

---

## 6. Fertigation logic

- Each schedule entry has a `fertigate` bool. Default scheduler **policy:** the first enabled run of each calendar day is marked `fertigate=true`, all others `false`. Policy is overridable per entry (`fertOverride: auto|on|off`).
- `RunController` sets the diverter legs per `fertigate` at run start (PREP_DIVERTER), skipping the travel when the legs are already in the wanted state (avoids needless ~6 s travel + wear). No cached position — the rest state is defined by the NO/NC valve types.
- Manual runs default `fertigate=false` unless explicitly requested.
- **Implemented (#27 policy / #28 actuation):** the one-fert-run/day policy + `auto|on|off` per-entry override live in the `Scheduler` (`resolveFert`, day boundary off the `Clock`); `RunController` sets the diverter in the §4 `PrepDiverter` step from `RunRequest::fertigate`. Fail-dry holds with no master: the diverter is set *before* `START_PUMP`, and the **pump** (the source) gates water, so a fert decision never holds water on. (The v1.4 two-leg diverter + the drop of the cached-position machinery landed in the task 1.8 rework — DEC-013.)

---

## 7. Flow monitoring (`FlowMonitor`)

- ISR increments a volatile pulse counter on `FLOW_PIN`.
- `pulsesPerGallon` (float K) stored in NVS; seeded from a datasheet default, overwritten by calibration.
- `gallons = pulses / K`; `rateGPM` from a rolling window.
- **During RUNNING**, after `FLOW_GRACE_S` (default 20): if `rateGPM` ≈ 0 → `FAULT_NO_FLOW` (clog, dead pump, valve never opened). Optionally fault on rate far outside an expected band.
- **During IDLE**: if accumulated pulses exceed `IDLE_FLOW_FAULT_PULSES` over a window → `FAULT_UNEXPECTED_FLOW` (stuck-open valve, burst). On this fault, re-assert safe state immediately (pump off, valves de-energized) and latch. **Post-run drain grace (#124):** each IDLE entry first waits for the meter to quiesce (≤ `DRAIN_QUIET_PULSES` over `DRAIN_QUIET_MS`, capped at `DRAIN_CAP_MS`) before the check arms — real hydraulics trail (draindown, slow valve seat) well past the IDLE edge, and judging the tail latched nuisance faults on wet runs. Flow that never decays still latches: the cap arms the check regardless, bounding detection at `DRAIN_CAP_MS` + one idle window (pump off and relay de-armed throughout).
- All runs log measured gallons.
- **Manual override (DEC-015):** a stored flag disables both flow faults (web UI). When set, `FlowMonitor` still measures and reports flow but never raises `FAULT_NO_FLOW` / `FAULT_UNEXPECTED_FLOW`, and enabling it clears any latched flow fault. Software-only — it cannot touch the watchdog or the pump-power source gate. Default off; a status flag + a persistent "⚠ FLOW CHECK DISABLED" UI banner show when it's active.
- **Implemented (#57):** the mute lives in `FlowFaultDetector::setMuted` (core, host-tested): verdicts go quiet, tracking does not — the idle window keeps sliding while muted, so un-muting judges a fresh window, never the muted backlog. The flag persists in NVS (`flow_ovr`, default = checks ON) and surfaces as `flow.overrideActive` in `/api/status` (the Unit D banner reads it). Enabling clears a latched flow fault via the sanctioned `requestClear()` path (the override mutes the condition, so the gate passes); non-flow faults keep their gates — the watchdog path is untouchable from here by construction.
- **Implemented (#34):** `FlowMonitor` (core) consumes a monotonic pulse count (ESP32 ISR + counter in `src/esp32/flow_sensor.h` via `attachInterruptArg`; injected in host tests). `gallons = (pulses − baseline)/K`; `rateGPM` from a ~1 Hz rolling ring that decays to 0 when flow stops (the signal #35's no-flow check keys on). K (`pulsesPerGallon`) loads from NVS via `Persistence` (float key, datasheet seed, overwritten by calibration #36). `main` re-baselines on the RUNNING edge and logs per-run gallons. The calibration state machine (#36) builds on this.
- **Implemented (#35):** `FlowFaultDetector` (core, `flow_fault_detector.{h,cpp}`) is the §7/§14 policy on top of `FlowMonitor`. Each tick `main` calls `update(runState, rateGPM, pulses, now)`; a non-`None` verdict is routed to `RunController::raiseFault` (the sole commander — the detector never actuates). The two checks are tied **explicitly** to run state, not the per-run tally edge: `FAULT_NO_FLOW` only during `RUNNING` after `graceMs` (`FLOW_GRACE_S`) at `rateGPM ≤ minRunningGPM`; `FAULT_UNEXPECTED_FLOW` only during `IDLE` when pulses over a tumbling `idleWindowMs` exceed `IDLE_FLOW_FAULT_PULSES`. Every transition state (Prep/Open/Start/Stop/Close/Settle) arms neither check, since flow legitimately ramps/trails there — and the trailing doesn't stop at the IDLE edge (#124): a `DrainGate` (core, `drain_gate.h`, shared with `ValveRestMonitor`) holds the idle check unarmed on each IDLE entry until the flow quiesces or `DRAIN_CAP_MS` expires. Host-tested with injected pulse patterns + a fake clock (grace arming, no-flow trip, idle-flow trip, per-run grace reset, transition-state immunity, trailing-draindown immunity + still-armed-after, never-decaying flow still latches, end-to-end raiseFault→safe-state). The manual override (DEC-015, above) is a separate task that will gate the verdict.

### Calibration mode
- `POST /api/calibrate/start {zoneIndex}` → opens that zone's path (diverter plain, zone open, pump on), zeroes the pulse counter, enters a bounded calibration run (own max-runtime).
- User collects output in a known container.
- `POST /api/calibrate/finish {measuredGallons}` → `K = pulsesCounted / measuredGallons`, store to NVS, close everything. Reject absurd values (sanity bounds).
- **Implemented (#36):** `CalibrationController` (core, `calibration_controller.{h,cpp}`) — the state machine + K math, host-tested; actuation routes through `RunController` (sole commander). `start(zone)` requires both idle (a calibration never queues), requests a bounded run (`CAL_RUN_SEC`, its own ceiling under `swMaxRuntimeSec`) with the diverter plain, and baselines its **own** pulse tally (independent of `FlowMonitor`'s per-run accumulation, so the RUNNING-edge re-baseline can't disturb it). The tally tracks live until the run reaches SETTLE (zone closed, trailing close-travel flow already counted), then freezes — SETTLE not IDLE, because a queued run chains SETTLE→next without visiting IDLE. `finish(measuredGallons)` stops a still-active run, computes K, and on success writes NVS (`k_ppg`) + the live `FlowMonitor`; an absurd volume or out-of-range K raises `FAULT_CAL_RANGE` (§14 latch + safe state) and leaves K untouched — the Phase 4 endpoint range-validates input before calling in. `cancel()` aborts without judging; a fault during the run voids the calibration (the original fault owns the story). The start/finish/cancel HTTP endpoints land with the Phase 4 web API.

---

## 8. Persistence (NVS / Preferences)

Survives reboot and power loss:
- Schedule entries (§13 model).
- `pulsesPerGallon`.
- Manual default durations (per zone).
- `swMaxRuntimeSec` (software ceiling, < ATtiny hard ceiling).
- Fertigation policy + per-entry overrides.
- Flow-fault override flag (DEC-015).
- Wi-Fi credentials.
- Run log: `RunLog` ring (`RUNLOG_DEPTH` = 32 entries) as one packed `runlog` blob — 11 B/entry
  `{startEpoch, zone, durationSec, centigallons, flags(fert|result|clockWasValid), faultCode}`,
  write-on-change + debounce, rehydrate read-with-default (DEC-018; additive under DEC-008).
- Fault log: `FaultManager` ring (`LOG_SIZE` = 16) as one packed `faultlog` blob — 6 B/entry
  `{epoch, code, flags(clockWasValid)}`, mirroring `runlog` (write-on-change + debounce, rehydrate
  read-with-default; additive under DEC-008, no `schema_ver` bump). Each entry is epoch-stamped with a
  per-entry `clockWasValid` bit (pre-2025 guard ⇒ bit clear), so it survives reboot with a meaningful
  timestamp; surfaced via `/api/status` (§14) + `/api/history` (DEC-018). (#90 — closes the #72 RAM-only gap.)

(No cached diverter position — the two-leg NO/NC diverter has no hold-state to remember, DEC-013.)

Write-on-change, not per-loop. Debounce writes.

**Schema (DEC-008):** flat prefixed keys in one `tinkle` namespace, **not** a packed blob —
per-zone state uses zone-indexed keys (`z<N>_dur`) iterated over the runtime `zoneCount` and
read-with-default, so adding a zone post-V1 needs no migration. A single `schema_ver` int
gates *transforming* migrations only; additive fields (new zone, new defaulted scalar) do
not bump it. NVS keys cap at 15 chars (silent truncation) — key names are bounded at the
source. Phase 2.1 (#25) persists the scalars that exist today (per-zone default durations,
`swMaxRuntimeSec`); the rest of this list is filled by its owning module through the same
store as that module lands.

**Cached diverter position — removed in v1.4 (DEC-013).** Phase 2 (#28) wired a stored
diverter position (`assumeDiverter()` boot-seed + write-on-change) for the hold-position
3-way. The two-leg NO/NC diverter has no hold-state to remember — each run sets the legs from
the fert flag and the unpowered rest is plain water — so `div_pos` and the boot-seed are
dropped in the task 1.8 rework. (`swMaxRuntimeSec` is still stored-not-read-back until
RunController gains runtime config in Phase 4.)

---

## 9. Watchdog handshake (`Watchdog` + ATtiny85)

**ESP32 side:**
- Toggle `HEARTBEAT_OUT` every `HEARTBEAT_MS` (default 250) from the main loop's healthy path. If the loop stalls, toggling stops — that *is* the signal.
- Read `WD_TRIPPED_IN`, **qualified** (DEC-023): a raw assertion counts only after holding continuously for `TRIP_CONFIRM_MS` — the line is a pulled-up input sampled once per tick, and a single glitched read (pump-relay switching transients land exactly in the run-end tail; 2026-07-09 field event) must never produce a verdict. A confirmed trip during an active state **aborts the current run** (normal unwind, logged `Faulted`) and **preserves the queue** — `FAULT_WATCHDOG` is non-latching (like DEC-014's `ValveRest`): the relay already de-powered the pump, and each queued run re-arms fresh under its own `HARD_MAX_RUNTIME`, so a blocking latch buys no safety and costs the rest of the schedule.
- Maintain an independent **software** max-runtime per run (`swMaxRuntimeSec`), shorter than the ATtiny ceiling — defense in depth so the hardware trip is the rare backstop, not the primary stop.

**ATtiny85 side (separate sketch, specified here):**
- Inputs: heartbeat from ESP32. Output: `ARM` line driving the **safety relay** coil (NO, energize-to-pass).
- Hold `ARM` asserted only while: (a) a heartbeat edge has been seen within `HB_TIMEOUT_MS` (default 2000), **and** (b) continuous armed time has not exceeded `HARD_MAX_RUNTIME` (default 30 min), timed on the ATtiny's own clock.
- On either failure: de-assert `ARM` → relay opens → the pump loses 24V → no source → fail dry. Also assert `WD_TRIPPED_IN` to the ESP32.
- Require the ESP32 to actively signal "run starting / run ended" so the ATtiny knows when to apply the runtime ceiling — simplest encoding: the heartbeat is only emitted during armed/active periods, or use a second discrete "run active" line if a pin is free. **Pick one encoding and document it in the ATtiny sketch header.**
- ATtiny must default to **ARM de-asserted** on its own power-up/reset (fail-dry on watchdog reboot).
- **Implemented (#48/#49, Unit B / DEC-016).** The chosen encoding and the protocol decisions it forced:
  - **Heartbeat window = pump commanded** (`START_PUMP`/`RUNNING`), not "any active state". `Watchdog` (core, `watchdog.{h,cpp}`) toggles `HEARTBEAT_OUT` every `HEARTBEAT_MS` inside the window (first edge immediately on entry — the ATtiny arms well inside the relay's needs) and parks the line LOW outside it. Between queued runs the close-travel + settle + next prep/open gap (≥ ~21 s real, ≥ ~3 s sim) far exceeds `HB_TIMEOUT_MS`, so the ATtiny disarms and re-arms per run and `HARD_MAX_RUNTIME` is a true **per-run** ceiling — back-to-back queued runs can't accumulate into a spurious trip. A `static_assert` in `main.cpp` fences this invariant — `2*ZONE_TRAVEL_MS + settleMs > HB_TIMEOUT_MS` — so a §6.2 travel-timing tune can't erode the gap below the timeout (#101).
  - **ATtiny trip logic is `WatchdogTrip`** (core, header-only `watchdog_trip.h`), the exact unit compiled into both the `attiny85` binary and the native test runner (DEC-016 condition). Three states: DISARMED (rest/power-up), ARMED (edge seen within `HB_TIMEOUT_MS` and armed time < `HARD_MAX_RUNTIME`, on the ATtiny's own clock), LOCKOUT (ceiling exceeded).
  - **Heartbeat quiet while armed disarms silently** — no TRIPPED. A clean run end is indistinguishable from a stalled ESP32 on this encoding, and a stalled ESP32 can't read the line anyway; opening the relay is the entire response. **TRIPPED asserts only on the `HARD_MAX_RUNTIME` lockout** — the one unambiguous, actionable fault (the ESP32 is alive and still claiming run-active past its own software stop). Lockout holds while the heartbeat keeps coming and releases to DISARMED after `HB_TIMEOUT_MS` of quiet — the ESP32 having aborted the run (DEC-023), safed, and gone silent. That released line is what the §4 pre-open gate waits on before the next run proceeds.
  - **Trip line is active-LOW, open-drain emulated** (ATtiny: INPUT = released, OUTPUT-LOW = asserted; the ESP32-side 10k pull-up idles it HIGH). A 5 V ATtiny can't overvolt GPIO36, and an absent/unpowered watchdog reads "not tripped" — the line is informational; the relay is the safety. ESP32 verdict (DEC-023): the raw read feeds `Watchdog`'s qualifier, and only a `TRIP_CONFIRM_MS`-continuous assertion confirms; a confirmed trip during any active state routes to `RunController::abortRun` — the current run unwinds and logs `Faulted`, the queue survives, nothing latches (`raiseFault(Watchdog)` is structurally refused). Confirmed-while-IDLE is not a fault: the §4 pre-open gate holds new runs until the line self-releases (skip-with-log past `WD_WAIT_MS`). `TINKLE_SIM` forces the trip read clear — Wokwi floats the unconnected input low, which would read as tripped.
  - ATtiny pin map (the sketch header is the source of truth for the ATtiny side): PB2 = HEARTBEAT_IN, PB1 = ARM_OUT (active high; external pulldown on the relay driver covers the Hi-Z reset window), PB0 = TRIPPED_OUT. Bench note: heartbeat is a 3.3 V swing — run the ATtiny at 3.3 V or level-shift (marginal against 0.6·Vcc at 5 V).

---

## 10. Web config API

Async server (ESPAsyncWebServer). STA-join the farm mesh using stored creds; if none/unreachable, fall back to SoftAP (`Tinkle-Setup`) with a captive config page. JSON over REST; a minimal SPA or server-rendered pages are both fine — keep it phone-usable in the field.

Endpoints:
- `GET /api/status` → current state, active zone, countdown sec, diverter state (plain/fert), live flow GPM, flow-override flag, last run summary, latched faults, clock/sync state. Polled for the live UI.
- `GET /api/schedule` · `POST /api/schedule` → full schedule array.
- `GET /api/settings` · `POST /api/settings` → default durations, `swMaxRuntimeSec`, `pulsesPerGallon`, fert policy.
- `POST /api/run` `{zoneIndex, durationSec?, fertigate?}` → enqueue a manual run.
- `POST /api/stop` → cancel all, unwind to safe state.
- `POST /api/calibrate/start` `{zoneIndex}` · `POST /api/calibrate/finish` `{measuredGallons}`.
- `POST /api/fault/clear` → clear latched faults (only succeeds if condition resolved).
- `GET /api/history` → the `RunLog` run ring (DEC-018) + the fault-log ring + a clock-valid flag. **Read-only** — no FAULT gate, no range validation. Lazy-fetched by the History screen, **not** on the `/api/status` poll. Payload ≈ 6 KB worst case at full depth (32 runs + the fault ring), still well under the transfer budget.
- `POST /api/ota` (#126, DEC-022) → firmware image upload, streamed chunk-by-chunk to `Update.write()` (its own route — the generic 4 KB body cap doesn't apply). Gated: accepted only from **IDLE or FAULT** with an empty queue (`Api::postOtaBegin`, core policy), and the accept sets `RunController`'s OTA inhibit so **no run — scheduled or manual — can start mid-flash**; a failed/dropped upload lifts the inhibit, a good one ends in `ESP.restart()` from the loop. Optional `X-OTA-Key` header auth (build-time secret via untracked `ota_secret.ini`). 200 flashed/rebooting · 401 bad key · 409 wrong state · 500 write/validation failure. `Update.end(true)` validates the image before the boot slot flips — a truncated upload never becomes the boot target (the stock arduino-esp32 bootloader has no rollback; USB stays the recovery path for a valid-but-broken image). `GET /api/status` gains `build` (a `<sha>[-dirty]-<UTC timestamp>` identity, injected by `tools/fw_build_id.py`; the timestamp makes it change on every flash — #159) so a flash is observable from the phone.

All mutating endpoints validate ranges and reject if currently in FAULT (except `/stop` and `/fault/clear`). `GET /api/history` is read-only and ungated.

- **Implemented (#55/#56/#57, Unit C / DEC-016).** Policy and plumbing are split on the platform line:
  - **`Api` (core, `api.{h,cpp}`)** owns everything decidable — range validation, FAULT gating, JSON shapes, the DEC-015 clear-on-enable — and is host-tested against the real wire shapes (ArduinoJson compiles on native; the tests parse/assert actual JSON, no parallel model). Codes: 200 OK · 400 malformed · 409 wrong state · 422 out of range. Validation seeds (tune like §15): run/schedule durations 10–7200 s, `swMaxRuntimeSec` 60–1800 s (never above the ATtiny ceiling), K 50–5000.
  - **One documented deviation:** `POST /api/settings` is allowed in FAULT — settings command no actuation, and DEC-015's enable-while-latched *is* the recovery path for a lying flow sensor. `/api/fault/clear` routes through `FaultManager::requestClear()` (the resolved-condition gate applies to the HTTP clear — the only clear path now that the button is gone, DEC-019). `/api/stop` also voids an active calibration (operator bailed: no K, no CalRange).
  - **Schedule** posts are atomic full-replaces: validate everything, then `clear()`+`add()`+NVS save (packed 10-byte entries, `sched` blob) + `evalNow()`. The blob rehydrates at boot — the schedule runs headless off flash (§17 local autonomy).
  - **`WebServer` (esp32, `web_server.h`)** is routes + body collection + one FreeRTOS mutex: async handlers run on the other core, so every `Api` call serializes against the loop (loop holds the lock per pass, releases between passes; handlers answer 503 after 250 ms rather than block the TCP task). Bodies cap at 4 KB (413).
  - **`WifiManager` (esp32, `wifi_manager.h`)**: non-blocking STA join with NVS creds → 20 s timeout → open SoftAP `Tinkle-Setup`; mDNS `tinkle.local` either way. Creds written via `/api/settings` apply at next boot. WiFi has no role in watering. **Post-join STA-drop recovery (#147):** once joined, the link is watched every tick; a sustained drop (> ~8 s, debounced) triggers `WiFi.reconnect()` retried every ~15 s **indefinitely** and re-announces mDNS on re-association — it does **not** fall back to SoftAP on a mid-season drop, so the box self-heals onto the returning farm AP rather than stranding off-network. Tradeoff: a *permanent* AP change (new SSID/password) after the first join is only escaped by a power-cycle (which re-runs the initial join → SoftAP fallback).
  - **`GET /api/status` additions** beyond the list above: fault-log ring (§14), `valveRestFlags` (DEC-014), calibration phase, schedule-drop counter, wifi mode/ip/rssi, uptime.

### 10.1 Served web interface (SPA) — part of the build

The firmware **serves a single-page app** that is the phone UI; it is a first-class deliverable, not optional. The SPA is a thin client that only calls the §10 API — no business logic lives in the browser. Phone joins the farm mesh, opens `http://tinkle.local` (mDNS) or the device IP, and gets the interface. Fully local; no internet dependency.

**Build & delivery constraints (ESP32 is the host):**
- **Vanilla HTML/CSS/JS only** — no React/Vue/build-step framework. The whole UI is a small static bundle.
- Serve from flash: embed as a single gzipped `index.html` (inline CSS + JS) in PROGMEM/LittleFS, served with `Content-Encoding: gzip`. Target the whole bundle **under ~50 KB gzipped**.
- No external CDN/font/script fetches (no internet in the tunnel). Everything self-contained.
- Mobile-first, large touch targets, high-contrast, readable in sunlight with wet hands. One-handed operation.
- Degrade gracefully: if an API call fails, show the last known state and a clear "disconnected" banner rather than a blank screen.

**Screens (all driven by the existing API):**
1. **Status / home** — current state, active zone, big MM:SS countdown (the sole countdown now — DEC-019 cut the TM1637), live flow GPM, diverter state (plain/fert), last-run summary, a persistent banner if the flow check is disabled (DEC-015), and any latched fault prominently. Polls `GET /api/status` (~1–2 s while active). Default screen.
2. **Manual run** — pick zone, duration (defaults pre-filled), fertigate toggle → `POST /api/run`; a big **STOP ALL** button → `POST /api/stop` always visible.
3. **Schedule editor** — list/add/edit/delete entries (time, zone, duration, days-of-week, fert override, enable) → `GET`/`POST /api/schedule`.
4. **Settings** — default durations, software max-runtime, fert policy, Wi-Fi → `GET`/`POST /api/settings`.
5. **Calibration** — guided flow-sensor calibration: start (`/api/calibrate/start`), prompt to measure collected volume, finish (`/api/calibrate/finish`); shows resulting pulses/gallon.
6. **Faults** — current latched faults + recent fault-log entries; clear button → `POST /api/fault/clear`.
7. **History** (DEC-018) — browsable run log: each past run with *when* (wall-clock if the clock was synced when it ran, else relative-to-uptime — the entry carries a `clockWasValid` bit), zone, MM:SS, gallons, fert y/n, result (ok / faulted + code); plus recent fault entries. Lazy-fetches `GET /api/history` on open + a manual refresh; **not** polled. Read-only — no actuation, no display role.

**Boundaries the SPA must respect:**
- The SPA can never bypass the API or touch actuation directly — same validation and FAULT gating applies.
- The SPA has **no role in fail-dry**. Watchdog, safety relay, and the pump-power gate are hardware; losing the phone, the page, or Wi-Fi must never affect safety or stop a scheduled run (the schedule lives in flash and runs headless). The flow-fault override (DEC-015) is the one SPA-reachable safety-adjacent setting, and it is firmware-gated and cannot touch any of that hardware.
- Treat the SPA as one of potentially several API clients (curl, future tooling); it gets no special privileges.
- **Implemented (#58/#59, Unit D / DEC-016).** One self-contained `web/index.html` (inline CSS+JS, no external fetches): six screens behind a bottom tab bar (History is the 7th, added by DEC-018 / [#71](https://github.com/mobiustripper42/tinkle/issues/71)), max-contrast light theme for sunlight, STOP ALL fixed above the tabs on every screen (never confirms). Status polls at 2 s idle / 1 s active with a local countdown tick between polls; fetch failure flips the persistent DISCONNECTED banner over last-known state. The DEC-015 "FLOW CHECK DISABLED" and latched-fault banners ride the same poll. Calibration screen carries the operator guidance (any zone into a measured container; Zone 3 preferred when plumbed; common-line meter ⇒ one K for all zones) and walks idle → running (live pulses) → awaiting → K. Faults screen renders the §14 log ring + DEC-014 `valveRestFlags` as per-zone service warnings. **Dev/mock:** the mock API is in-page — `file://` or `?mock=1` runs every screen against an in-memory stub. **Pipeline (#59):** `tools/build_spa.py` runs as a PlatformIO pre-action on every esp32/esp32_sim build — deterministic gzip (mtime=0) into the generated, gitignored `src/esp32/spa_gz.h`; the <50 KB budget is a hard build gate (currently ~26 KB raw → ~8.6 KB gzipped); served from PROGMEM with `Content-Encoding: gzip` + ETag/`no-cache` (reflash ⇒ fresh UI, unchanged ⇒ one 304).

---

## 11. Operator interface — phone-only (DEC-019)

V1.5 has **no on-box controls**. The three zone buttons that lived here (DEC-006: idle press
started that zone, any press stopped a running zone, a ≥3 s long-press cleared a latched fault)
are **cut**. All manual action — **start, stop, fault-clear** — is the SPA over the device's own
Wi-Fi/SoftAP (§10 / §10.1), routed into the same `RunController` / `FaultManager` seams the buttons
used to hit. The only physical control is the **AC master switch** on the Mean Well input: a
whole-system kill → fail-dry (purely electrical, no firmware), doubling as the service disconnect.
The `Buttons` module is removed from the build but git-recoverable if a panel is ever resurrected.

> The single-active invariant, the fert-default, and Zone 3's build-for-three status are unchanged
> — only the *input path* moved to the phone. See DEC-019 for the stop-vs-start/clear tradeoff.

---

## 12. Status indication — alive LED + SPA (DEC-019)

The TM1637 4-digit panel (idle `HH:MM` clock, `MM:SS` countdown, `E#` fault codes) and the three
button LED rings are **cut**. Their job — run state, countdown, active zone, latched faults — now
lives entirely in the SPA status/home screen (§10.1), which polls `GET /api/status`.

The only on-board indicator is a single **alive LED** (`ALIVE_LED`, the DevKitC onboard LED on
GPIO2) blinking ~1 Hz to show the firmware is still ticking. It is **read-only and gates nothing**,
and is **distinct from** the ATtiny `HEARTBEAT_OUT` watchdog handshake (§9) — do not conflate. The
`Display` core module + the TM1637 esp32 shim are removed from the build, git-recoverable.

---

## 13. Clock & scheduling

- NTP via `configTime` on Wi-Fi join. On no network, free-run from last known time + `millis()` (drift acceptable for irrigation; document it). Optional future DS3231 RTC — leave a clean seam (`Clock` interface) but don't require it for V1.
- **Implemented (#26 / DEC-009):** `Clock` core sits over an injected `IWallClock` whose contract is **local epoch seconds**; the ESP32 `SystemClock` shim owns timezone + DST (`configTzTime` with a POSIX TZ rule, re-packing `localtime_r` fields via the pure `epochFromCivil`). Core anchors a synced reading to a `millis()` instant and free-runs between reads, re-syncing hourly; `valid()` feeds the display's `clockValid` (false → "--:--" until NTP lands). `minuteRolled()` is the per-minute eval edge the `Scheduler` consumes below. A DST flip while the network is down isn't reflected until the next resync (accepted, DEC-009).
- Schedule entry model:

```
struct ScheduleEntry {
  uint8_t  id;
  uint8_t  zoneIndex;
  uint8_t  hour, minute;      // wall clock
  uint16_t durationSec;
  uint8_t  daysMask;          // bit per weekday; 0x7F = daily
  uint8_t  fertOverride;      // auto|on|off
  bool     enabled;
};
```

- `Scheduler` checks once per minute (and on edit) for due entries, enqueues runs, applies the fert policy (§6), and prevents overlap by queueing. A run that would collide with an active run waits, with a max queue depth; drop + log if exceeded.
- **Implemented (#27 / DEC-010):** in-memory `Scheduler` (core) evaluates entries on each new local minute via the `Clock`, keyed on the absolute minute so it runs at most once per minute (idempotent against the DEC-009 resync nudge); `evalNow()` covers "on edit." Due runs are enqueued through the narrow `IRunSink` seam (`RunController` implements it) and dropped+counted when refused — overlap is the `RunController` queue's job, not the scheduler's. Fert policy (§6): the first **auto** run of each calendar day fertigates, the slot consumed only on a successful enqueue; `On`/`Off` overrides bypass the slot. Schedule entries are **not persisted yet** — deferred to the Phase 4 web editor that will own save-on-edit and mirror them to NVS (DEC-008).

---

## 14. Fault handling & safe state

- Faults latch. Entering any fault: command safe state (pump off → all valve FETs de-energized: zones cap-close, diverter returns to plain), set fault code, surface it on the SPA status/Faults screen (§10.1), log. (No on-box fault display under DEC-019 — the alive LED keeps blinking; it is not a fault indicator.)
- Recovery requires explicit clear (`/api/fault/clear` — phone-only since DEC-019) **and** the underlying condition resolved.
- Fault codes: `FAULT_NO_FLOW`, `FAULT_UNEXPECTED_FLOW`, `FAULT_CAL_RANGE`, `FAULT_CLOCK` (optional/non-blocking). **Non-latching codes:** `FAULT_WATCHDOG` (DEC-023 — aborts the current run, preserves the queue; the relay is the safety) and `FAULT_VALVE_REST` (DEC-014 — log-only maintenance flag). Both are structurally refused by `raiseFault`.
- Hierarchy of trust: software stops first; the ATtiny + safety relay **cutting pump power** is the hardware backstop (the source gate, DEC-012). Firmware must never assume it is the only thing keeping water off.
- **Implemented (#50, software half).** `FaultManager` (core, `fault_manager.{h,cpp}`) on top of `RunController`'s latch: a fixed ring of recent fault entries (code + millis timestamp — the §10 status/Faults surface), and the **resolved-condition clear gate**. Detectors push their condition's current truth in each tick (`setConditionActive`): unexpected-flow resolved = no flow while idle (the rolling rate decayed to 0); the watchdog condition is still pushed for the log surface, though `FAULT_WATCHDOG` no longer latches (DEC-023). One-shot codes nobody pushes (`FAULT_CAL_RANGE`) clear freely. The clear path — `POST /api/fault/clear` (phone-only since DEC-019 retired the button long-press) — goes through `requestClear()`, so a latched-but-unresolved fault is a visible no-op (the SPA reports the clear didn't take), not a false clear. The physical safety-relay wiring half of 5.3 is bench/parts-gated and verifies under §17 (#51).
- **Implemented (#52, DEC-014).** `ValveRestMonitor` (core, `valve_rest_monitor.{h,cpp}`) — the auto-return self-test. After a run's close travel, it first waits out the trailing draindown (#124, `DrainGate`): the rest window opens only once the meter quiesces (≤ `DRAIN_QUIET_PULSES` over `DRAIN_QUIET_MS`), so a healthy-but-slow-seating valve isn't flagged on the tail of its own run; flow that never quiets by `DRAIN_CAP_MS` flags the zone directly (water still passing long past any draindown is the failure itself). It then watches the flow meter through SETTLE/IDLE for `REST_WINDOW_MS`: more than `REST_MAX_PULSES` means the just-closed zone still passes water → that zone is flagged. **Non-latching by design** — the flag becomes a `FaultManager::note()` ring entry (`FAULT_VALVE_REST`, E6, log-only) + a serial line + `flaggedMask()` for the Phase 4 status API, never a `raiseFault` (a dribble must not halt irrigation; a gross leak still latches via the §7 idle check, a decade above this threshold). The flag **self-heals**: a later clean rest window on the same zone clears it. A queued run chaining SETTLE→next aborts the check silently (the window would measure the next run's flow) — the last run of a queue session is still checked, so every used zone gets covered. Diverter legs are out of scope for V1 (no flow signature at rest); the §17 bench item exercises the zone path.

---

## 15. Default constants

| Constant | Default | Notes |
|---|---|---|
| `ZONE_TRAVEL_MS` | 10000 | NC valve open/close travel; bench-confirm (datasheet 6–10 s). Replaces `PULSE_MS` — task 1.8 |
| `DIVERTER_TRAVEL_MS` | 10000 | diverter-leg travel (same valve family) |
| `HEARTBEAT_MS` | 250 | ESP32 → ATtiny toggle |
| `HB_TIMEOUT_MS` | 2000 | ATtiny trip on lost heartbeat |
| `TRIP_CONFIRM_MS` | 100 | DEC-023: continuous trip-line assertion required before a read counts (a real lockout holds ≥ 2 s; kills single-sample glitches) |
| `WD_WAIT_MS` | 60000 | DEC-023: pre-open gate holds this long for a trip-line release before skipping the run (self-release is spec'd ≤ 2 s) |
| `HARD_MAX_RUNTIME` | 30 min | ATtiny ceiling (own clock) |
| `swMaxRuntimeSec` | 1200 | ESP32 per-run ceiling, configurable |
| `FLOW_GRACE_S` | 20 | settle before no-flow check |
| `IDLE_FLOW_FAULT_PULSES` | 139 (K≈1670-referenced ≈ 1.0 GPM; `TINKLE_SIM`: 50) | unexpected-flow threshold over one idle window (`FlowFaultDetector`, #35). ~1 GPM rejects post-run draindown while a welded relay (~1.45 GPM) still trips; at-grade catchment (#141) |
| `CAL_RUN_SEC` | 120 (seed; `TINKLE_SIM`: 10) | calibration run ceiling, its own bound under `swMaxRuntimeSec` (#36) |
| cal sanity bounds | K ∈ [50, 5000] p/gal, ≥ 0.25 gal | reject absurd calibrations → `FAULT_CAL_RANGE` (#36; seeds, tune) |
| `REST_WINDOW_MS` | 10000 (seed; `TINKLE_SIM`: 3000) | DEC-014 post-close rest window (#52) |
| `REST_MAX_PULSES` | 5 (seed, tune) | pulses at rest above which the closed zone is flagged (#52) |
| `DRAIN_QUIET_MS` | 3000 (seed; `TINKLE_SIM`: 1000) | #124 drain gate: a sub-window this long must stay quiet before the idle-flow check / rest window arms |
| `DRAIN_QUIET_PULSES` | 2 (seed, tune) | #124: max pulses tolerated in one quiet sub-window (~0.02 GPM at K≈1670) |
| `DRAIN_CAP_MS` | 60000 (seed; `TINKLE_SIM`: 5000) | #124: bound on the drain wait — the idle check arms regardless (burst still latches); `ValveRestMonitor` flags the zone directly (still flowing = the DEC-014 failure) |
| `RUNLOG_DEPTH` | 32 | run-history ring entries (DEC-018); 11 B/entry packed `runlog` NVS blob (~356 B); bumpable to 64 (wear, not space, is the ceiling) |
| `DIST_RUN_FLOOR_MIN` | 7 | DEC-024 Distributed Watering: hard minimum run length; the day's per-zone budget splits into runs no shorter than this |
| `DIST_MAX_RUNS` | 6 | DEC-024: max cycles/runs per zone per day (the fired-cycle bitmask is per-day) |
| `DIST_RUN_OVERHEAD_SEC` | 25 | DEC-024: per-run valve/pump/settle overhead used in the cycle-fit check (estimate pending `ZONE_TRAVEL_MS` confirm, #98) |

Bench-confirm `ZONE_TRAVEL_MS` and `DIVERTER_TRAVEL_MS` against the actual parts before trusting the defaults.

---

## 16. Suggested stack

PlatformIO + Arduino-ESP32. Libraries: ESPAsyncWebServer + AsyncTCP, ArduinoJson, Preferences (NVS), `configTime`/SNTP, ESPmDNS (for `tinkle.local`), LittleFS (or PROGMEM) to hold the gzipped SPA bundle. (No TM1637 driver — DEC-019 cut the display.) Custom thin state-machine code (avoid heavyweight RTOS framing for V1). ATtiny85 sketch built via Arduino-as-ISP or micronucleus if a Digispark board is used; keep it dependency-free and tiny.

---

## 17. Acceptance checklist

- [ ] Power loss at any run step leaves the system dry (pump unpowered → no source; valve state irrelevant) with no firmware involvement.
- [ ] Pulling the heartbeat (halt firmware) trips the safety relay within `HB_TIMEOUT_MS` and de-powers the pump.
- [ ] A run exceeding `HARD_MAX_RUNTIME` is cut by the ATtiny even if the ESP32 believes the run is fine.
- [ ] No-flow during a run faults and safes within a few seconds past the grace window.
- [ ] Unexpected idle flow faults and re-safes immediately.
- [ ] Fert policy marks exactly one run/day THROUGH the Dosatron unless overridden; the diverter legs travel only on change.
- [ ] Calibration mode writes a sane K and survives reboot.
- [ ] Schedule executes with Wi-Fi pulled (local autonomy).
- [ ] Web UI shows live countdown + lets a phone start/stop/calibrate in the field.
- [ ] SPA is served gzipped from flash, loads with no internet, and a dead phone/Wi-Fi never affects a running or scheduled irrigation.
- [ ] Every valve channel rests LOW (off) at boot — NC zones closed, NO bypass open (plain), pump off.
- [ ] Each valve closes within `ZONE_TRAVEL_MS` of de-energize; the auto-return self-test (DEC-014) flags a valve that doesn't.
- [ ] The flow-fault override (DEC-015) mutes faults without touching the watchdog / source gate; the "flow check disabled" banner shows when active.
- [ ] A power loss in the first ~60 s after boot still leaves the system dry via the source gate (caps not yet charged — but the pump is the gate).
- [ ] An OTA in progress refuses any run request — scheduled or manual — until reboot; the pump cannot energize mid-flash (#126, DEC-022). A truncated/corrupt upload is discarded (old image keeps booting) and lifts the run inhibit.
- [ ] A watchdog trip — real or spurious — costs at most the CURRENT run (DEC-023): the queue survives, the next run holds at the pre-open gate until the line releases, and no later scheduled run is blocked. A sub-`TRIP_CONFIRM_MS` glitch on the trip line produces no verdict at all.
