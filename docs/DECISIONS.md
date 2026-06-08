# Tinkle — Architectural Decisions

Decisions are numbered DEC-NNN. "DEC-TBD" means the decision is flagged but
unresolved — consult @architect before building.

The hardware and firmware specs (`docs/tinkle_v1_spec.md`,
`docs/tinkle_firmware_spec.md`, `docs/tinkle_wiring.html`) carry the detailed
rationale for component selection and behavior. This file records the
project-shape and architecture decisions made during planning.

---

## DEC-001: One repo, two MCU build environments
**Decision:** A single repository with one `platformio.ini` carrying three envs —
`esp32` (firmware), `attiny85` (watchdog), and `native` (host unit tests). Sources
split by directory: `src/esp32`, `src/attiny`, `src/core` (platform-independent
logic), selected per env via `build_src_filter`.
**Why:** The ESP32 and ATtiny are bound by a shared contract — the watchdog
handshake (DEC-004). Keeping both binaries in one repo keeps that contract in one
place and one history. `src/core` holds the safety-critical logic so it compiles
into both the firmware and the host test runner.
**Tradeoff:** Two toolchains in one project; `build_src_filter` must stay correct
so the wrong sources don't leak into an env.

## DEC-002: SPA embedded as a gzipped PROGMEM bundle
**Decision:** The phone UI is vanilla HTML/CSS/JS, a single `index.html` gzipped
and embedded into flash (PROGMEM), served with `Content-Encoding: gzip`. No
filesystem partition, no build-step framework.
**Why:** No FS partition to manage, the UI ships atomically with the firmware
image, and a tiny static bundle (< 50 KB gzipped) is trivial to serve offline.
**Tradeoff:** A UI change requires a reflash. Acceptable for V1 — the UI is the
field-edit path, not a frequently-iterated product surface. LittleFS (OTA-able UI)
is the escape hatch if that changes.

## DEC-003: Single-wire watchdog encoding — heartbeat means "run active"
**Decision:** The ESP32 emits its heartbeat square wave **only while a watering
run is active**. "Heartbeat present" therefore means "a run is in progress."
There is no separate run-active line. The ATtiny holds the safety relay armed
only while (a) a heartbeat edge was seen within `HB_TIMEOUT_MS` and (b) continuous
armed time is under `HARD_MAX_RUNTIME`, timed on its own clock.
**Why:** The firmware spec §9 left the run-active encoding open and the wiring doc
(§B) allocates **no** run-active pin while captioning the heartbeat as "while
firmware healthy" — those two can't coexist (an always-on heartbeat gives the
ATtiny no way to bound run time). This resolves the inconsistency with **zero
hardware change**: keep the exact pin map, redefine the heartbeat semantics, and
document them in the ATtiny sketch header (done, `src/attiny/main.cpp`).
**Why it's safe:** Idle crash → no heartbeat → relay de-armed, which is the safe
state we want anyway (idle = no water; a missed cycle is harmless). Loop stall
mid-run → heartbeat stops → trip in ~2s. Logic-bug run-forever with the loop alive
→ software ceiling (`swMaxRuntimeSec`, 20 min) fires first; the ATtiny's 30-min
hard ceiling is the backstop. The ATtiny can never *cause* water — it only gates
power.
**Tradeoff:** The heartbeat conflates "firmware alive" with "run active." That
conflation is sound here precisely because de-arming during idle is desired.
**Update (DEC-012):** with the master valve removed, this idle-disarm is the **fail-dry
backbone**, not just a runtime-ceiling convenience — the safety relay gates the **pump** (the
water source), so "no heartbeat → relay de-armed → pump unpowered → no water" is the primary
guarantee. An always-on heartbeat would remove the no-source-at-idle property and is prohibited.

## DEC-004: Firmware-first development; sim and bench before flash
**Decision:** Build and validate in tiers, deferring real hardware. (1) Native
unit tests on the host for `src/core` logic with fake clock + fake GPIO. (2) Full-
firmware simulation in Wokwi on the laptop. (3) Breadboard bench with LEDs standing
in for valves/master/pump and a pulse source faking the flow sensor — runs the full
§17 acceptance checklist. (4) Wet hardware (pump, valves, Dosatron, tape) as the
final confirm gate only.
**Why:** The flash/reflash loop is slow and the wet hardware arrives last (build
target Winter 2026–27). Most logic — including the safety-critical state machine —
can be exercised off-hardware. Bench stand-ins validate real silicon before water.
**Tradeoff:** Bench-tunable constants (`ZONE_TRAVEL_MS`, `DIVERTER_TRAVEL_MS`, flow
K-factor) stay at seeded defaults until tiers 3–4 confirm them.

---

## DEC-005: TM1637 display driver — `robtillaart/TM1637_RT`
**Decision:** Use `robtillaart/TM1637_RT` for the 4-digit panel (§12), replacing the
delisted `avishorp/TM1637` (commented out in #20). Glyph *logic* (countdown format,
fault codes, blink/flash phases, dashes) lives in host-tested `src/core/display`
producing ASCII glyphs + a colon flag; the esp32 shim (`display_tm1637.h`) hands those
to `displayPChar`, whose encoder maps `0-9`/`E`/`-`/space 1:1, and rides the colon on
the high bit of cell 1.
**Why:** Flat, dependency-free C++ class — builds clean under `-std=gnu++11` (the
PR #18 lockstep). Thin char + `setBrightness` surface keeps the testable logic in core
and the lib isolated. Maintained (0.4.3, 2026). Rejected `akj7/TM1637` (templated →
higher gnu++11 audit cost; its non-blocking-animation feature is moot when we render
frames ourselves) and git-pinning dead `avishorp` (reintroduces the supply fragility
#20 removed).
**Tradeoff / guard:** Every software TM1637 driver bit-bangs the 2-wire protocol with
`delayMicroseconds` (verified: no `delay()` in the write path; default bit delay 10 µs
→ a 4-digit frame is ~low single-digit ms, bench-confirm). To stay under the ≤10 ms tick (§2), the
shim pushes to the display **only when the rendered frame changes** (minute rollover,
second tick, colon/fault-flash edges — all ≤2 Hz), never every loop. Brightness, the
exact per-frame cost, and the colon cell position are bench-confirmable (Phase 6).

---

## DEC-006: Button/zone model — 3 buttons = 3 zones, any-press-stops, long-press-clears
**Decision:** The panel is **three buttons, one per zone**, with no dedicated stop
button (resolves #23). Behavior (§11):
- **IDLE** → press button N starts Zone N at its stored default duration, `fertigate=false`.
- **Any zone running** → a press of *any* button **stops** it (graceful unwind to safe
  state). It does **not** switch or auto-start — to change zones, one press stops, the
  next starts. This trivially enforces the single-active invariant (you must stop before
  you can start).
- **FAULT** → a short press is a no-op; a **≥3 s long-press of any button** requests a
  fault-clear, still gated on "condition resolved" (§14) — a premature clear simply
  re-faults on the next run, harmless. The hold must give **explicit feedback** (§12): a
  ring/display ack on a successful clear, and a *visible* no-op (not silence) when held
  while latched-but-unresolved, so a held button never reads as a dead panel.
- Zone 3 is a real third zone — a **general-purpose hose outlet** separate from the Red
  Tunnel's Z1/Z2 — under build-for-three: wired now (3rd NC motorized ball valve + low-side
  FET, per DEC-011), plumbed when that line goes in.
**Why:** This is Eric's literal directive ("each button runs its own zone, any button
cancels"). The earlier §11 design (B1/B2 = zones, B3 = dedicated Stop/cancel-all +
long-press clear) was a spec error, not the intent. The any-press-stops rule is
fail-dry-friendly: an explicit stop, never a surprise switch that starts water you
didn't ask for. The press-overload (start / stop / clear, keyed on {state, hold
duration}) resolves with no ambiguity — in FAULT only the long-press acts.
**Tradeoff / guard:** Fault-clear spreads across three buttons (was one), multiplying
the *surface* for an accidental hold but not its *probability* (a 3 s hold is
deliberate), and §14's re-fault-on-next-run makes a premature clear harmless. The button
only *requests* a guarded state transition — it can never command water — so the
fail-dry chain (sw ceiling → ATtiny → pump-power gate, DEC-012) is untouched. The web `/api/fault/clear`
(§10) stays as the parallel path; the button preserves local autonomy at the enclosure.
**Status:** Implemented Phase 1.7 (#23). The "unresolved-hold visible no-op" feedback
branch is **gated on the FaultManager resolved-condition signal (Phase 3/5)** — until
then `clearFault()` clears unconditionally when faulted and only the success ack fires.

---

## DEC-007: Zone 3 H-bridge on strapping pins GPIO15/GPIO12
**Decision:** Build-for-three needs a third zone H-bridge, but every non-strapping
output GPIO is already spent (wiring doc §B). Assign **Z3_IN1 = GPIO15 (MTDO)** and
**Z3_IN2 = GPIO12 (MTDI)**.
**Why:** Safe *only* because the DRV8871's ~100 kΩ internal input pulldowns hold both
ESP32 pins **LOW** through the boot strapping window while the pins are still hi-Z:
GPIO12 sampling low selects the correct **3.3 V flash VDD** (a HIGH here bricks boot —
the pulldown is doing real work), GPIO15 low is cosmetic (suppresses the U0TXD boot
log), and both-low means **no spurious valve travel at boot** (fail-dry). This mirrors
the existing master-FET gate-pulldown boot note — an established pattern here.
**Rejected:** GPIO0 (pulldown → download mode at boot), GPIO5 (must be HIGH at boot,
pulldown fights it), GPIO2 (works, but its onboard LED flickers on every Z3 actuation),
and an I/O expander (adds an I2C/SPI dependency, a part, and a failure mode to dodge a
cosmetic boot-log — against build-for-three/populate-one).
**Constraint:** **Nothing may pull GPIO12 high** — no external pull-up, no scope probe
with a pull, nothing. Documented in the wiring doc §D so the next person doesn't brick
boot without knowing why. (Supersedes the wiring doc's prior "no Z3 pins allocated.")
**Retired (DEC-011/012):** the v1.4 valves are on/off (1 GPIO each, no H-bridge), so there is
no Z3 H-bridge and no IN1/IN2 pair on the strapping pins — GPIO12/15 are freed and this
brick-boot hazard is gone. A valve FET that lands on a strapping pin still needs a gate-pulldown
for boot-off (the master-FET pattern), but the specific GPIO12/15 allocation is withdrawn. See
the v1.4 pin map (wiring doc §B).

## DEC-008: NVS persistence — per-zone-indexed keys, read-with-default, single schema_ver
**Decision:** `Persistence` (§8) stores state as flat, prefixed keys in one `tinkle`
Preferences namespace (`z<N>_dur`, `sw_max_sec`, `div_pos`), iterated over the runtime
`zoneCount` (single source: `ValveConfig::MAX_ZONES` for capacity, the injected count for
iteration — never a second constant), each read with a default. A single `schema_ver` int
(=1) gates migrations.
**Why:** Zones will be added after V1 (the controller is sized for three tunnels and grows
from there). A fixed-width 3-zone struct blob would force a migration the moment a 4th zone
lands; per-zone-indexed keys make a new zone "iterate further, default the missing key" —
zero migration. `schema_ver` is what read-with-default *can't* give you: it distinguishes
"key absent, fresh install" from "key absent, a transforming migration must run." Rule:
additive changes (a new zone, a new defaulted scalar) are absorbed by read-with-default and
**do not** bump `schema_ver`; only a field whose meaning/encoding changes does. NVS keys cap
at 15 chars (Preferences silently truncates longer) — the `z<N>_dur` formatter is bounded at
the source so a two-digit zone can never alias a sibling.
**Scope (Phase 2.1 / #25):** ships the `IKeyValueStore` abstraction + `PreferencesStore`
shim + `FakeKeyValueStore`, persisting the three scalars that exist today — per-zone default
durations (retires the `BUTTON_RUN_SEC=600` placeholder in `main.cpp`), `swMaxRuntimeSec`,
and cached diverter position — with write-on-change (a set to the current value touches no
flash). Schedule entries (#27), Wi-Fi creds (Phase 4), the fault-log ring (#3/5), and
`pulsesPerGallon` (Phase 3) are deferred hooks: each owning module persists through the same
store with its own keys, deliberately not pre-carved here.
**Supersedes:** the Session 3 note to "lock the NVS schema around `ZONE_COUNT=3`."
**Update (#28):** the deferred *cached diverter position* wiring landed — boot-seeded into
`ValveDriver` (`assumeDiverter()`, no travel) and written back via a write-on-change poll in
`main.cpp`. The `swMaxRuntimeSec` mirror remains stored-not-read-back until RunController
gains runtime config (Phase 4).
**Update (DEC-011/013):** the two-2-way diverter has **no hold-position to cache** — fert state
is set per-run by energizing the correct leg (legs rest by their NO/NC type). The `div_pos` key
and the `assumeDiverter()` boot-seed are removed in the task 1.7 rework; the per-zone duration +
`swMaxRuntimeSec` keys are unaffected.

---

## DEC-009: Clock — local-epoch seam, timezone/DST in the ESP32 shim, hourly resync
**Decision:** `Clock` (§13) is platform-independent core over an injected `IWallClock`
seam whose contract is **local epoch seconds** — UTC already offset for the farm's
timezone and DST. The ESP32 binding (`SystemClock`, `src/esp32/system_clock.h`) owns all
of that: `configTzTime` installs a POSIX TZ rule (`EST5EDT,M3.2.0/2,M11.1.0/2`) + the
SNTP servers, `localtime_r` applies it, and the local broken-down fields are re-packed
into a local epoch via the pure, host-tested `epochFromCivil` (ESP32 newlib omits
`tm_gmtoff`, so adding an offset isn't an option). Core's only jobs: anchor an
authoritative reading to a `millis()` instant and **free-run** between reads
(`epoch = anchor + (millis()-anchorMs)`), track `valid()` (synced ≥ once since boot) for
the display's `clockValid`, and derive local HH:MM + weekday. Polling is throttled — brisk
(1 s) until lock-on so the clock snaps valid soon after WiFi appears, then a 1 h re-anchor
so the free-run path is genuinely exercised and NTP corrects its drift.
**Why:** Keeping timezone/DST in the shim leaves the core a pure epoch→fields derivation
with no timezone database — fully host-testable with a fake source and explicit epochs,
and the future DS3231 RTC drops in as just another `IWallClock` with no core change (§13
"clean seam"). `configTzTime` handles DST transitions automatically, avoiding a fixed
offset that would be an hour wrong half the year.
**Limitation (accepted):** a DST flip while the network is **down** is not reflected until
the next resync re-anchors — sub-hour, on a clock whose drift §13 already calls
"acceptable for irrigation." Before the first NTP sync the wall clock is unknown:
`valid()` is false and the display holds "--:--" (matching boot behavior).
**Scope (Phase 2.2 / #26):** `Clock` core + `SystemClock` shim + a `FakeWallClock`, wired
to the display's `clockValid`/HH:MM in `main.cpp` (retires the hardcoded `false`). Exposes
`wall()`/`epoch()` and a `minuteRolled()` per-minute edge for the Scheduler's per-minute
eval (#27). `configTzTime` is called in `setup()` for now; Phase 4 relocates/​re-invokes it
on the WiFi-join event per §13.

---

## DEC-010: Scheduler — IRunSink seam, minute-keyed idempotent eval, entries not yet persisted
**Decision:** `Scheduler` (§13) is in-memory core that evaluates entries on each new **local
minute** from the `Clock`. Three calls:
- **`IRunSink` seam:** callers that only *request* runs (Scheduler, and the Phase 4 web API)
  depend on a narrow `IRunSink { requestRun() }` interface; `RunController` implements it.
  Overlap is **not** the scheduler's problem — `RunController` already queues sequential runs
  and rejects when full (§4); a due run that can't enqueue is dropped and counted (§13).
- **Idempotent eval:** evaluation is keyed on the absolute local minute (`epoch/60`) and is
  **forward-only** (`curMin <= lastEvalMin_` skips) — each minute is evaluated at most once,
  ever. This drops the DEC-009 hourly-resync backward nudge even when it steps across a minute
  boundary into an already-run minute (an `==` guard would miss that). `evalNow()` re-arms the
  key to cover "on edit" (§13).
- **Fert policy (§6):** the first `Auto` run of each calendar day fertigates; the daily slot
  is consumed **only on a successful enqueue** (a queue-full rejection doesn't burn the day's
  fert). `On`/`Off` overrides force the diverter state and bypass the slot.
**Why:** The `IRunSink` seam matches the project's injection idiom (`IGpio`/`IKeyValueStore`/
`IWallClock`) and lets the scheduler's eval + fert + overlap logic be host-tested against a
fake sink, in isolation from the run state machine. Keeping overlap in `RunController` avoids
a second queue with its own bugs.
**Deferred — entry persistence:** schedule entries are held in RAM only. There is no editor
until the Phase 4 web config API, so there is nothing to persist; that API will own
save-on-edit and mirror entries to NVS through the Persistence store's own keys (DEC-008).
The §13 entry model + engine land now (`add`/`clear`/`count`/`entry`); the NVS keys land with
the thing that edits them.
**Scope (Phase 2.3 / #27):** `Scheduler` core + a `FakeRunSink`, `MAX_ENTRIES=16`, wired into
the loop in `main.cpp` (no-op until the schedule is populated and the clock is valid). Fert
**actuation** still flows through `RunController`'s existing diverter handling; #28 layers any
remaining fert-policy nuance on top.

---

## DEC-011: Valve architecture — US Solid 2-wire auto-return ball valves, FET-driven, no H-bridges
**Decision:** Every actuated valve in V1 is a **U.S. Solid 3/4" brass 2-wire auto-return
motorized ball valve** (9–36 V AC/DC, ~2 W, 6–10 s travel, **capacitor** return), switched
on/off by a **discrete low-side N-FET** (IRLZ44N) — one GPIO per valve, **no H-bridges, no
DRV8871, no reverse-polarity drive**. The set:
- **Zones (Z1, Z2, Z3): normally-closed (NC)** — closed when de-energized, energize to open.
- **Diverter = two 2-way valves** (replacing the 3-way — DEC-013): **NO** clean/bypass leg +
  **NC** Dosatron leg, plus a check valve on the Dosatron outlet.

A **single pressure regulator per tunnel** sits **upstream** of the zone valves (≤15 psi).
Supersedes the v1.1 Hunter PGV + 458200 latching scheme **and** the abandoned v1.3
reverse-polarity hold-position framing (wrong part — the as-sourced valve is on/off
auto-return, not reverse-polarity). Recorded in the hardware spec's v1.4 changelog.
**Why:** The actual part is on/off (energize to actuate, cap auto-return on de-energize), so it
drives like the old master — a single FET, not an H-bridge. That collapses the system to one
valve family on one driver type, **deletes every H-bridge** and the never-both-high invariant,
and frees the pin map (1 GPIO/valve, not 2) — which **retires DEC-007**. NC zones rest closed
and the NO bypass rests open, so the whole system's unpowered resting state is "plain-water path
open, everything else closed" — the correct safe default.
**Why it's safe:** the **pump-power gate (DEC-012), not any valve,** is the fail-dry barrier, so
the valves' resting states are a convenience, not the safety mechanism.
**Driver / clamp (sourcing):** IRLZ44N per valve (margin makes the cap-inrush spec moot), gate
resistor + gate-to-GND pulldown (boot-off), and a **TVS (SMAJ30A) drain-to-source per FET** —
the valves have an internal bridge rectifier, so a freewheel diode won't clamp; clamp the FET.
Valves on **raw 24 V**; button LED rings are 24 V (12 V buck dropped).
**Firmware impact (tracked, not done — re-scoped task 1.7):** `ValveDriver` still ships the
H-bridge/latching-pulse API (`pulseOpen`/`pulseClose`, IN1/IN2 pairs, `PULSE_MS`). The rework is
now a real re-architecture — on/off channels with per-valve travel timers (`driveOpen`/
`driveClose` = set/clear a FET), `ZONE_TRAVEL_MS` ≈ 10 s, drop the never-both-high invariant,
`pins.h` re-map (1 pin/valve, master pin removed), and the native tests. The run state machine
already gates on `zoneBusy()`, so the sequencing seam survives; the driver underneath changes.
~5–8 pts (was a 3-pt rename). Travel time is a bench-confirmed seed (DEC-004).

## DEC-012: No master valve — fail-dry by source control
**Decision:** V1 has **no master valve**. The fail-dry barrier is **source control**: the pump
sits on the **armed 24 V** (downstream of the ATtiny safety relay, DEC-003), energized only
during an active run. No run / hang / watchdog trip / power loss → pump unpowered → no pressure
→ no water, regardless of any valve's state. Removes the WIC 2BCW NC solenoid + its FET channel.
**Why:** With NC zones and a demand pump, the master's only *unique* job was blocking a gravity
siphon through a stuck-open zone while the pump is off — and that path doesn't exist here:
near-zero tank head plus the SEAFLO's internal check valves block reverse flow (confirmed).
During a run the master would be open anyway; between runs the pump is already unpowered by the
safety relay. So the master duplicated a barrier the pump-power gate already provides. Dropping
it removes a part, ~18 W, and a continuous-duty-coil sourcing constraint with **no loss of real
protection**.
**Why it's safe:** the source gate is firmware-independent (ATtiny + relay), fail-safe (NO relay
de-energizes open → no power), and runs are time-bounded by `HARD_MAX_RUNTIME`, so even a wrong
or extra open valve during a run waters a bounded amount, never a runaway. The valves are **not**
a safety barrier; their resting-closed state is for agronomic correctness (which bed gets water),
caught by the self-test (DEC-014) and the flow cross-check (firmware spec §7).
**Amends DEC-003 — now load-bearing:** the safety relay gates the **pump** (was "master + pump"),
and DEC-003's "heartbeat = run-active" idle-disarm becomes the fail-dry backbone. An always-on
heartbeat would remove the no-source-at-idle guarantee — so that encoding is now a safety
requirement, not just a runtime-ceiling convenience.

## DEC-013: Dosatron diverter — two 2-way valves, not a 3-way
**Decision:** The motorized 3-way diverter is replaced by **two 2-way ball valves** (the DEC-011
family): a **normally-open** valve on the clean/bypass leg and a **normally-closed** valve on the
Dosatron leg, with a **check valve** (existing GASHER 3/4" brass) on the Dosatron **outlet**,
between the injector and the rejoin tee.
**Why:** With every valve already this on/off family, two 2-ways are simpler than a special
reverse-polarity 3-way and unify the part list. The NO/NC pairing makes the **unpowered rest
state = plain water flows, Dosatron isolated** — exactly one path open, so **no both-closed
deadhead** (which is why the hardware spec's old "single 3-way preferred over two 2-way solenoids"
note is superseded: that reasoned about NC+NC). A **plain run powers nothing**; a fert run
energizes the NC leg open + the NO leg closed. No cached diverter position is needed (simplifies
DEC-008).
**Why it's safe:** the diverter is upstream of the zones and the source gate, so any diverter
state (both-open, both-closed, wrong leg) is at worst a fertigation error, never a flood. The
check valve stops bypass flow back-feeding the idle injector. A leg switch briefly overlaps (~5–15
s travel) — harmless (pump rides a short deadhead on its internal bypass; brief split just
under-injects); firmware may stagger make-before-break (optional).

## DEC-014: Auto-return self-test — correctness, not a safety barrier
**Decision:** The firmware periodically verifies each valve **rests closed** when it should
(commanded close watched on the flow sensor / position indicator) and flags a valve that fails.
**Why:** The auto-return is capacitor-driven — it needs ~1 min of power after the valve opens to
charge, and a cap ages over years in heat, so a valve *could* fail to close on de-energize. This
is **not** a fail-dry concern (the pump-power gate, DEC-012, is the barrier; a stuck-open valve
passes nothing with the pump off), but it **is** an agronomic-correctness and maintenance concern
— don't silently water the wrong bed, and get a heads-up to service a degrading valve. The
self-test is the detector; it is explicitly **not** credited as a safety layer.

## DEC-015: Flow-check manual override (web UI)
**Decision:** A web-UI setting disables the FlowMonitor **faults** (`NO_FLOW` and
unexpected-idle-flow) so a missing, uncalibrated, or misbehaving flow sensor can't block
watering. Default = checks on; enabling clears any latched flow fault; persisted in NVS.
**Why:** Fail-dry cuts both ways — a bad sensor that halts all irrigation is its own failure. The
override **mutes faults, not measurement** (flow is still counted + displayed) and is
**software-only**: it cannot touch the watchdog, safety relay, pump-power gate, or
`HARD_MAX_RUNTIME`. Worst case is a run finishing its commanded duration without the cross-check —
bounded, never a runaway (consistent with firmware spec §10.1: the SPA has no role in fail-dry).
While active, every screen shows a persistent "⚠ FLOW CHECK DISABLED" banner + a status flag + a
fault-log entry. V1 = single toggle (mute both); granular (keep the leak detector always on) is a
later refinement.
