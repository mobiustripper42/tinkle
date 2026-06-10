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
- Drive a TM1637 countdown display and three button LED rings.
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
- `Buttons` — debounce, edge events.
- `Display` — TM1637 rendering of state.
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
// Display
TM_CLK=25  TM_DIO=26
// Buttons (external pull-up, active-low) — one per zone, no dedicated stop (DEC-006)
BTN1=34  BTN2=35  BTN3=39
// Button LED rings — 24V rings, one low-side switch each
LED1=32  LED2=33  LED3=23
// Watchdog
HEARTBEAT_OUT=4
WD_TRIPPED_IN=36
// Free for more zones (build-for-three): 19, 21, 5, 2, 12, 15
```

Each FET gate gets a series resistor + a gate-to-GND pulldown so the valve sits **off**
(closed/rest) through ESP32 boot. Model zones as a table so the count is data-driven:

```
struct Zone { uint8_t fetPin, ledPin, btnPin; const char* name; };
Zone zones[] = {
  {Z1_FET, LED1, BTN1, "Zone 1"},   // Red Tunnel beds 1–3
  {Z2_FET, LED2, BTN2, "Zone 2"},   // Red Tunnel beds 4–6
  {Z3_FET, LED3, BTN3, "Zone 3"},   // general-purpose hose outlet (build-for-three)
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
2. **OPEN_ZONE** — energize the zone FET (NC valve drives open); wait `ZONE_TRAVEL_MS`. The pump waits for this (the state machine gates on the valve no longer being busy), so the pump never loads against a closed/mid-travel valve. If `WD_TRIPPED_IN` is asserted, abort to FAULT.
3. **START_PUMP** — close `PUMP_RELAY`. Begin `RUNNING`.
4. **RUNNING** — run for `durationSec`. Continuously: update countdown display, run flow checks (§7), watch software max-runtime. Any fault → jump to STOP_PUMP path then FAULT.
5. **STOP_PUMP** — open pump relay (this is what stops water — the source).
6. **CLOSE_ZONE** — de-energize the zone FET; the cap auto-return drives it closed over `ZONE_TRAVEL_MS`. (Pump is already off, so the valve closes against no pressure.)
7. **SETTLE** — brief dwell, log the run (zone, start, duration, gallons, fert y/n, result). Diverter legs left as-is. → IDLE (or next queued run).

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
- **During IDLE**: if accumulated pulses exceed `IDLE_FLOW_FAULT_PULSES` over a window → `FAULT_UNEXPECTED_FLOW` (stuck-open valve, burst). On this fault, re-assert safe state immediately (pump off, valves de-energized) and latch.
- All runs log measured gallons.
- **Manual override (DEC-015):** a stored flag disables both flow faults (web UI). When set, `FlowMonitor` still measures and reports flow but never raises `FAULT_NO_FLOW` / `FAULT_UNEXPECTED_FLOW`, and enabling it clears any latched flow fault. Software-only — it cannot touch the watchdog or the pump-power source gate. Default off; a status flag + a persistent "⚠ FLOW CHECK DISABLED" UI banner show when it's active.
- **Implemented (#34):** `FlowMonitor` (core) consumes a monotonic pulse count (ESP32 ISR + counter in `src/esp32/flow_sensor.h` via `attachInterruptArg`; injected in host tests). `gallons = (pulses − baseline)/K`; `rateGPM` from a ~1 Hz rolling ring that decays to 0 when flow stops (the signal #35's no-flow check keys on). K (`pulsesPerGallon`) loads from NVS via `Persistence` (float key, datasheet seed, overwritten by calibration #36). `main` re-baselines on the RUNNING edge and logs per-run gallons. The calibration state machine (#36) builds on this.
- **Implemented (#35):** `FlowFaultDetector` (core, `flow_fault_detector.{h,cpp}`) is the §7/§14 policy on top of `FlowMonitor`. Each tick `main` calls `update(runState, rateGPM, pulses, now)`; a non-`None` verdict is routed to `RunController::raiseFault` (the sole commander — the detector never actuates). The two checks are tied **explicitly** to run state, not the per-run tally edge: `FAULT_NO_FLOW` only during `RUNNING` after `graceMs` (`FLOW_GRACE_S`) at `rateGPM ≤ minRunningGPM`; `FAULT_UNEXPECTED_FLOW` only during `IDLE` when pulses over a tumbling `idleWindowMs` exceed `IDLE_FLOW_FAULT_PULSES`. Every transition state (Prep/Open/Start/Stop/Close/Settle) arms neither check, since flow legitimately ramps/trails there. Host-tested with injected pulse patterns + a fake clock (grace arming, no-flow trip, idle-flow trip, per-run grace reset, transition-state immunity, end-to-end raiseFault→safe-state). The manual override (DEC-015, above) is a separate task that will gate the verdict.

### Calibration mode
- `POST /api/calibrate/start {zoneIndex}` → opens that zone's path (diverter plain, zone open, pump on), zeroes the pulse counter, enters a bounded calibration run (own max-runtime).
- User collects output in a known container.
- `POST /api/calibrate/finish {measuredGallons}` → `K = pulsesCounted / measuredGallons`, store to NVS, close everything. Reject absurd values (sanity bounds).

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
- Fault log: ring buffer (~16 entries) of `{ts, code, context}`.

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
- Read `WD_TRIPPED_IN`. If asserted: immediately force safe state, enter `FAULT(FAULT_WATCHDOG)`, latch until manual clear.
- Maintain an independent **software** max-runtime per run (`swMaxRuntimeSec`), shorter than the ATtiny ceiling — defense in depth so the hardware trip is the rare backstop, not the primary stop.

**ATtiny85 side (separate sketch, specified here):**
- Inputs: heartbeat from ESP32. Output: `ARM` line driving the **safety relay** coil (NO, energize-to-pass).
- Hold `ARM` asserted only while: (a) a heartbeat edge has been seen within `HB_TIMEOUT_MS` (default 2000), **and** (b) continuous armed time has not exceeded `HARD_MAX_RUNTIME` (default 30 min), timed on the ATtiny's own clock.
- On either failure: de-assert `ARM` → relay opens → the pump loses 24V → no source → fail dry. Also assert `WD_TRIPPED_IN` to the ESP32.
- Require the ESP32 to actively signal "run starting / run ended" so the ATtiny knows when to apply the runtime ceiling — simplest encoding: the heartbeat is only emitted during armed/active periods, or use a second discrete "run active" line if a pin is free. **Pick one encoding and document it in the ATtiny sketch header.**
- ATtiny must default to **ARM de-asserted** on its own power-up/reset (fail-dry on watchdog reboot).

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

All mutating endpoints validate ranges and reject if currently in FAULT (except `/stop` and `/fault/clear`).

### 10.1 Served web interface (SPA) — part of the build

The firmware **serves a single-page app** that is the phone UI; it is a first-class deliverable, not optional. The SPA is a thin client that only calls the §10 API — no business logic lives in the browser. Phone joins the farm mesh, opens `http://tinkle.local` (mDNS) or the device IP, and gets the interface. Fully local; no internet dependency.

**Build & delivery constraints (ESP32 is the host):**
- **Vanilla HTML/CSS/JS only** — no React/Vue/build-step framework. The whole UI is a small static bundle.
- Serve from flash: embed as a single gzipped `index.html` (inline CSS + JS) in PROGMEM/LittleFS, served with `Content-Encoding: gzip`. Target the whole bundle **under ~50 KB gzipped**.
- No external CDN/font/script fetches (no internet in the tunnel). Everything self-contained.
- Mobile-first, large touch targets, high-contrast, readable in sunlight with wet hands. One-handed operation.
- Degrade gracefully: if an API call fails, show the last known state and a clear "disconnected" banner rather than a blank screen.

**Screens (all driven by the existing API):**
1. **Status / home** — current state, active zone, big MM:SS countdown (mirrors the TM1637), live flow GPM, diverter state (plain/fert), last-run summary, a persistent banner if the flow check is disabled (DEC-015), and any latched fault prominently. Polls `GET /api/status` (~1–2 s while active). Default screen.
2. **Manual run** — pick zone, duration (defaults pre-filled), fertigate toggle → `POST /api/run`; a big **STOP ALL** button → `POST /api/stop` always visible.
3. **Schedule editor** — list/add/edit/delete entries (time, zone, duration, days-of-week, fert override, enable) → `GET`/`POST /api/schedule`.
4. **Settings** — default durations, software max-runtime, fert policy, Wi-Fi → `GET`/`POST /api/settings`.
5. **Calibration** — guided flow-sensor calibration: start (`/api/calibrate/start`), prompt to measure collected volume, finish (`/api/calibrate/finish`); shows resulting pulses/gallon.
6. **Faults** — current latched faults + recent fault-log entries; clear button → `POST /api/fault/clear`.

**Boundaries the SPA must respect:**
- The SPA can never bypass the API or touch actuation directly — same validation and FAULT gating applies.
- The SPA has **no role in fail-dry**. Watchdog, safety relay, and the pump-power gate are hardware; losing the phone, the page, or Wi-Fi must never affect safety or stop a scheduled run (the schedule lives in flash and runs headless). The flow-fault override (DEC-015) is the one SPA-reachable safety-adjacent setting, and it is firmware-gated and cannot touch any of that hardware.
- Treat the SPA as one of potentially several API clients (curl, future tooling); it gets no special privileges.

---

## 11. Manual buttons

**Three buttons, one per zone, no dedicated stop button** (DEC-006). B1/B2/B3
(GPIO34/35/39) map to Zone 1 / Zone 2 / Zone 3. One uniform policy keyed on
`{run state, hold duration}`:

- **IDLE → press button N:** start a timed run on Zone N at that zone's stored default
  duration, `fertigate=false`.
- **Any zone running → press *any* button:** **stop** — unwind the active run to safe
  state. It does **not** switch or auto-start; to change zones, one press stops, the next
  starts. This is what enforces the single-active invariant (you must stop before you can
  start) and is fail-dry-friendly — an explicit stop, never a surprise switch.
- **FAULT → short press:** no-op. **FAULT → ≥3 s long-press of any button:** request a
  fault-clear, still gated on "condition resolved" (§14); a premature clear simply
  re-faults on the next run (harmless). The hold **must give explicit feedback** — see
  §12 (a held button must never read as a dead panel).
- Firmware debounces (~30 ms) on top of the RC hardware debounce. Act on the press edge;
  the long-press fires once per hold at the 3 s threshold.

> **Zone 3** is a real third zone — a general-purpose hose outlet, separate from the Red
> Tunnel's Z1/Z2 — wired now under build-for-three (DEC-007), plumbed when that line goes
> in. B3 was previously (and wrongly) specced as a dedicated Stop button; that was a spec
> error corrected by DEC-006.

---

## 12. Display (TM1637)

- **IDLE:** wall clock `HH:MM`, colon on. (If clock unsynced, show `--:--`.)
- **RUNNING:** `MM:SS` countdown of the active run; colon blinks at 1 Hz.
- **PREP/SETTLE:** short animation or hold last value — minor, implementer's choice.
- **FAULT:** `E` + code, e.g. `E1` no-flow, `E2` unexpected flow, `E3` watchdog. Flash.
- Display is **read-only status** — it never gates actuation and has no input role.

Button LED rings: off = idle/that zone not running; solid = that zone running. With no
dedicated stop ring (DEC-006), **fault/attention blinks every ring** (the panel as a
whole says "attention"), overriding the per-zone level.

**Fault-clear hold feedback** (DEC-006): the ≥3 s fault-clear long-press must produce
explicit feedback so a held button never reads as a dead panel —

- **Successful clear** (latched → cleared) → a brief ring/display **ack** (all rings
  flash solid). *Today this ack means "was faulted, now cleared," not "the fault
  condition is gone"* — `clearFault()` currently clears unconditionally when faulted.
- **Held while latched-but-unresolved** → a *visible* no-op (a brief flash), not silence.
  This branch is **gated on the FaultManager resolved-condition signal (Phase 3/5)** —
  there is no "resolved" signal until FlowMonitor/Watchdog land, so it is speced here but
  not yet implemented.

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

- Faults latch. Entering any fault: command safe state (pump off → all valve FETs de-energized: zones cap-close, diverter returns to plain), set fault code, light attention LED + display code, log.
- Recovery requires explicit clear (`/api/fault/clear` or B3 long-press) **and** the underlying condition resolved.
- Fault codes: `FAULT_NO_FLOW`, `FAULT_UNEXPECTED_FLOW`, `FAULT_WATCHDOG`, `FAULT_CAL_RANGE`, `FAULT_CLOCK` (optional/non-blocking).
- Hierarchy of trust: software stops first; the ATtiny + safety relay **cutting pump power** is the hardware backstop (the source gate, DEC-012). Firmware must never assume it is the only thing keeping water off.

---

## 15. Default constants

| Constant | Default | Notes |
|---|---|---|
| `ZONE_TRAVEL_MS` | 10000 | NC valve open/close travel; bench-confirm (datasheet 6–10 s). Replaces `PULSE_MS` — task 1.8 |
| `DIVERTER_TRAVEL_MS` | 10000 | diverter-leg travel (same valve family) |
| `HEARTBEAT_MS` | 250 | ESP32 → ATtiny toggle |
| `HB_TIMEOUT_MS` | 2000 | ATtiny trip on lost heartbeat |
| `HARD_MAX_RUNTIME` | 30 min | ATtiny ceiling (own clock) |
| `swMaxRuntimeSec` | 1200 | ESP32 per-run ceiling, configurable |
| `FLOW_GRACE_S` | 20 | settle before no-flow check |
| `IDLE_FLOW_FAULT_PULSES` | 50 (seed, tune) | unexpected-flow threshold over one idle window (`FlowFaultDetector`, #35) |
| button debounce | 30 ms | on top of RC |

Bench-confirm `ZONE_TRAVEL_MS` and `DIVERTER_TRAVEL_MS` against the actual parts before trusting the defaults.

---

## 16. Suggested stack

PlatformIO + Arduino-ESP32. Libraries: ESPAsyncWebServer + AsyncTCP, ArduinoJson, Preferences (NVS), a TM1637 driver, `configTime`/SNTP, ESPmDNS (for `tinkle.local`), LittleFS (or PROGMEM) to hold the gzipped SPA bundle. Custom thin button + state-machine code (avoid heavyweight RTOS framing for V1). ATtiny85 sketch built via Arduino-as-ISP or micronucleus if a Digispark board is used; keep it dependency-free and tiny.

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
