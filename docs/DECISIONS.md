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
> **Superseded by DEC-019** (v1.5 phone-only): the TM1637 display is cut and this driver leaves
> `lib_deps`. Retained for history; the `src/core/display` glyph logic is git-recoverable.
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
> **Superseded by DEC-019** (v1.5 phone-only): the buttons are cut; manual start/stop/fault-clear
> are now SPA-only. The zone *model* (three zones, build-for-three) survives — only the button
> *input* is gone. DEC-006's "local autonomy at the enclosure" sub-claim is retired (see DEC-019).
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
and the `assumeDiverter()` boot-seed were removed in the task 1.8 rework; the per-zone duration +
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
resistor + gate-to-GND pulldown (boot-off), and a **TVS (1.5KE30CA, through-hole) drain-to-source
per FET** (SMAJ30A in the original v1.4 sourcing — updated to the through-hole 1.5KE30CA under
DEC-020) — the valves have an internal bridge rectifier, so a freewheel diode won't clamp; clamp the FET.
Valves on **raw 24 V**. (The button LED rings that previously shared the 24 V rail are gone under
DEC-019 — their load leaves the power budget.)
**Firmware impact (done — task 1.8):** `ValveDriver` and `pins.h` are now the on/off model —
`openZone`/`closeZone` set/clear one low-side FET per valve, per-valve travel timers
(`ZONE_TRAVEL_MS`/`DIVERTER_TRAVEL_MS` = 10 s seeds), the never-both-high invariant and the
`MASTER_FET` are gone, the diverter is two leg FETs, and the native tests were rewritten. The
run state machine kept its `zoneBusy()`/`diverterBusy()` gates; only the master open/close
states dropped. 8 pts (was first scoped as a 3-pt rename). Travel time is a bench-confirmed
seed (DEC-004 / §15).

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

## DEC-016: Phase 5 (watchdog) runs before Phase 4 (web)
**Decision:** Implement the watchdog/fail-dry chain (5.1–5.3) before the web API + SPA. Batches:
**Unit B** = 5.1+5.2+5.3-software as one PR (~16 pts) — both sides of the heartbeat contract, with
the ATtiny trip logic as a platform-independent unit compiled into both the `attiny85` and `native`
envs so host tests exercise both halves against the same §15 constants; the DEC-014 self-test ships
as a separate follow-on PR (~3 pts), not bundled. **Unit C** = 4.1+4.2+DEC-015 (web API layer);
**Unit D** = 4.3+4.4 (SPA + gzip pipeline). Task 5.4 (§17 bench pass) stays parts-gated.
**Why:** Phase 5 is the critical path (PROJECT_PLAN: fail-dry proven on the bench before any wet
run); FaultManager (5.3) shapes the fault surface `/api/status`, `/api/fault/clear`, and the SPA
Faults screen consume (firmware spec §10/§10.1, DEC-006's resolved-condition gate), so
watchdog-first is strictly less rework. (The original rationale added "bench work needs no UI —
buttons + TM1637 suffice"; **DEC-019** cut both, so bench and sim now drive through the SPA / `curl`
against the §10 API. The watchdog-first ordering is unaffected.)
**Tradeoff:** The phone UI arrives last among software phases; acceptable because wet validation
(the only consumer that *needs* it) is parts-gated to Winter 2026–27.

---

## DEC-017: Low-tank pump lockout — V2 integration seam with Soundings (not built)
**Status:** **V2, not built.** This records the *seam* so the V1 design leaves room for it;
the implementation is deferred and **@architect-gated** (the transport choice below is unresolved).
**Decision (shape, not implementation):** The Soundings mesh will grow a tank-cluster level sensor
(one A02YYUW ultrasonic node over the farm's three plumbed-together catchment tanks, 2530 gal —
see Soundings `DEC-005`/`DEC-006` and `docs/tank-level-sensor.md`), publishing
`farm/water/cluster/level_gal`, `.../percent`, and raw `.../distance_mm`. Tinkle consumes that level
to **lock the pump out before it runs the cluster dry**. The consumer seam is a future `src/core`
module — call it `TankMonitor` — that:
- gates `RunController::requestRun()` (reject a new run below a configurable threshold, like the
  FAULT gate already does), and
- raises a new `FAULT_LOW_TANK` if the level crosses the floor mid-run,
following the existing `FlowFaultDetector` pattern (push a fault through `RunController::raiseFault`)
with a FaultManager resolved-condition clear gate (level recovered), exactly as DEC-006's
fault-clear works.
**Why it fits cleanly:** the fault-detector seam (`FlowFaultDetector`, `ValveRestMonitor`) and the
`requestRun` precondition gate already exist — a tank lockout is another detector, not new
plumbing. Threshold persists through the DEC-008 store like `swMaxRuntimeSec`.
**Open question for @architect (why it's not built yet):**
1. **Transport.** MQTT subscribe (pulls in a broker dependency + AsyncMqttClient) vs. HTTP poll of
   the Soundings gateway vs. a **hardwired GPIO** from a local float/level switch. GPIO is the only
   option that respects **local autonomy** (DEC philosophy: watering never depends on the network);
   the MQTT/HTTP options make the lockout depend on the mesh being up.
2. **Fail-dry direction conflict.** A *missing* tank signal is ambiguous: lock out
   conservatively (protects the pump, but a dead sensor or down mesh then blocks *all* watering — the
   exact failure DEC-015 added the flow-check override to avoid) vs. allow watering (preserves
   autonomy, but exposes the pump to a dry run). The pump's dry-run is an **equipment** risk, not the
   flood risk the fail-dry chain (DEC-012) is built around — so this lockout is a *protection* layer,
   **not** part of the fail-dry safety chain, and must not be allowed to compromise it. The resolution
   (likely: GPIO float-switch + a DEC-015-style override) is an architecture decision, deferred.
**Crosses into Not V1:** "Closed-loop / sensor-driven irrigation" and "MQTT to the Soundings stack"
are already parked there (SPEC.md); this DEC names the specific lockout case and its seam.

---

## DEC-018: Run history — persisted `RunLog` ring, `GET /api/history`, 7th SPA screen
**Status:** **decided; built in Phase 4** ([#68](https://github.com/mobiustripper42/tinkle/issues/68)
gate/docs · [#69](https://github.com/mobiustripper42/tinkle/issues/69) core ·
[#70](https://github.com/mobiustripper42/tinkle/issues/70) API ·
[#71](https://github.com/mobiustripper42/tinkle/issues/71) SPA). @architect-ratified.
**Context:** "What ran when, and faults" wants a browsable history. The fault side already ships
(`FaultManager` ring → `/api/status` → Faults screen). The **run** side does not: every run is
logged at SETTLE (§4 step 7 — zone, start, duration, gallons, fert, result) but only the single
**last-run** summary is kept and exposed. The data is already produced; this persists and surfaces
the rest.
**Decision:**
- **`RunLog` (`src/core`, host-tested):** a fixed ring of **32** entries (`RUNLOG_DEPTH`, §15).
  `RunController` pushes one entry at SETTLE; the existing single "last run" becomes the ring **head**
  (one source of truth — `/api/status` `lastRun` reads `runlog[head]`).
- **Packed record, 11 bytes:** `startEpoch u32 · zoneIndex u8 · durationSec u16 · centigallons u16 ·
  flags u8 (fert | result | clockWasValid) · faultCode u8`. `centigallons` (uint16, 0–655.35 gal),
  **not** float — NVS-blob-portable and padding-free.
- **Persistence:** one packed `runlog` NVS blob (write-on-change + debounce, rehydrate at boot
  read-with-default → absent = empty ring), the same shape as the `sched` blob — **not** per-entry
  keys (15-char key cap, DEC-008) and **not** append-style (needless at a few runs/day; the real
  constraint is write-wear, which one debounced ~356 B blob/run barely touches). Additive under
  DEC-008: new key, no `schema_ver` bump. Depth 32 ≈ 356 B; `RUNLOG_DEPTH` is one constant, bumpable
  to 64 if the field asks (space is not the ceiling, wear is).
- **Timestamps:** store local `startEpoch` + a **per-entry** `clockWasValid` bit (NTP had synced when
  the run started — validity varies *across* the ring, so a single global flag won't do). The SPA
  renders wall-clock only when the bit is set, else relative-to-uptime (mirrors the fault log's
  `ago(uptimeMs − atMs)`). An implausible epoch (pre-2025) is stored with the bit **clear**, never as
  a 1970 wall-clock — this bounds the NTP-syncs-mid-history clock-jump: early free-run entries render
  relative, not silently mis-dated.
- **`GET /api/history`:** a new **read-only** endpoint (no FAULT gate — §10's gating rule is
  mutating-endpoints-only; no range validation), lazy-fetched when the History screen opens — **not**
  folded into the 1–2 s `/api/status` poll. `Api` (core) serializes the run ring + the fault ring + a
  clock-valid flag; host-tested against real JSON. Payload ≈ 6 KB worst case at full depth (32 runs
  + the fault ring), well under the transfer budget (`sendJson` has no outbound cap; `BODY_CAP` gates
  only inbound POST bodies).
- **7th SPA screen — "History":** a dedicated tab (runs list: wall-clock-or-relative, zone, MM:SS,
  gallons, fert, result/fault; plus fault entries), lazy fetch + manual refresh, DISCONNECTED degrade,
  mock-API rows. Amends the "six screens" wording in firmware §10.1 / SPEC Phase 4.
**Why it fits:** local telemetry, inside "local autonomy" — **not** the remote server/MQTT/DB/Grafana
class barred by SPEC "Not V1" (that line bars *remote* telemetry, not an on-device log). The read-only
endpoint off the hot path, the additive NVS key, and a read-only screen are the lowest-novelty
extension of four existing patterns (fault ring, `sched` blob, status poll, tab bar). No fail-dry
surface touched — the SPA has no safety role (§10.1) and a read path can't actuate.
**Scope guard — fault ring stays RAM-only:** §8 *described* the fault log as NVS-persisted ~16
entries, but the shipped `FaultManager` ring is RAM-only, `LOG_SIZE = 8`, millis-domain. This DEC does
**not** persist or migrate the fault ring — `/api/history` serializes it as-is. **Resolved (#72):**
§8 was corrected to the RAM-only reality, with the persistence feature deferred. **Shipped (#90):**
that feature then landed — `FaultManager` entries are now epoch-stamped (+ a `clockWasValid` bit) and
the ring persists as the `faultlog` NVS blob (`LOG_SIZE = 16`), mirroring `runlog`; §8 updated to match.
**Consequences:** a new `runlog` NVS key; `RunController` gains a SETTLE-time push; `/api/status`
`lastRun` becomes a view onto the ring head; the "six screens" line in two docs becomes "seven".

---

## DEC-019: Phone-only operator interface — physical panel cut for V1 (supersedes DEC-005, DEC-006)
**Decision:** Delete the on-box operator panel — the **TM1637 4-digit display** (§12), the **three
zone buttons** (DEC-006), and the **three button LED rings** — keeping **one** board-level
alive/health LED (the DevKitC onboard LED, GPIO2, `ALIVE_LED`) that blinks at ~1 Hz to show the
firmware is ticking. The **SPA** over the device's own Wi-Fi/SoftAP (§10.1) is now the **sole**
interface. This is **v1.5**.
**Why:** The panel is physical fabrication, parts, and enclosure cutouts Eric can't afford against
the deploy date — and the SPA already delivers every panel function it would replace: Manual-run +
the always-visible **STOP ALL** (§10.1 screen 2) cover the buttons; Status/History mirror the
display. PROJECT_PLAN already listed "1.6 TM1637 display" under *Cuttable Tasks* ("web UI shows the
same countdown"); this realizes that cut and goes one step further by also dropping the buttons.
**Supersedes DEC-005** (the `robtillaart/TM1637_RT` driver leaves `lib_deps`) and **DEC-006** (the
3-button model). **Amends DEC-016**, whose "bench needs no UI — buttons + TM1637 suffice" rationale
is now false: bench and sim drive through the SPA / `curl` against the §10 API.
**Local autonomy is unchanged — and SPEC's claim stands as written.** Scheduled runs still execute
headless from local flash regardless of network (the load-bearing guarantee, untouched), and the
phone reaches the box over the ESP32's **own SoftAP** (`Tinkle-Setup`, §10) — local I/O, not
infrastructure — so phone-only adds **no** network / server / cloud dependency. What is retired is
the *narrower* sub-claim only DEC-006 ever made: "the button preserves local autonomy **at the
enclosure**," i.e. finger-only operation standing at the box with no phone.
**The honest tradeoff (stop vs. start/clear asymmetry):**
- Phoneless **stop** is *preserved and broadened*: a new **AC master switch on the Mean Well input**
  is a whole-system kill → fail-dry (purely electrical, **no firmware involvement**), documented as
  the **service disconnect** and the phoneless emergency stop. It realizes §17 item 1 (power loss →
  pump unpowered → dry) as a deliberate act — a hard cut, not a graceful unwind, which the design
  already treats as a first-class fail-dry path.
- Phoneless **start** and phoneless **fault-clear** are *gone*: a dead phone in the truck means you
  can't hand-start Zone 2 or clear a latched fault at the box. §14's two clear paths collapse to one
  (`/api/fault/clear` only; the B3 long-press is removed). Removing a *resume* path can only make
  the system more conservative, never less dry.
**Safety chain untouched (DEC-003 / DEC-012).** The buttons only *requested* guarded transitions and
the display was read-only (§12) — neither was ever in the pump-power fail-dry gate, and the §17
acceptance checklist has **no** panel dependency (its lone UI item is the SPA). The `ALIVE_LED` is a
local health blink, **distinct from** `HEARTBEAT_OUT` (GPIO4), the ATtiny watchdog handshake — do
not conflate the two.
**Deferred, not deleted.** The `Buttons` / `Display` core modules + the `display_tm1637.h` shim are
removed from the build but remain **recoverable from git history** if a panel is ever resurrected
(no current plans). The freed GPIO (ex-display 25/26, ex-rings 23/32/33, ex-buttons input-only
34/35/39) bank toward the build-for-three future zones — no remap needed for Red.
**Status:** Decided + built (task 5.7). Firmware: modules deleted, `pins.h` reshaped (`Zone` sheds
`ledPin`/`btnPin`), `main.cpp` drops the button policy / display / ring render for the alive blink,
`TM1637_RT` out of `lib_deps`; native suite 109/109; `esp32` / `esp32_sim` / `attiny85` green. The
SPA-driven e2e sim (the former button-driven Wokwi scenarios) is redone separately (issue #62).

---

## DEC-020: v1.5 hardware reconcile — power tree, pump, flow sensor, clamp, enclosure
**Decision:** Six as-built hardware corrections, settled in the design chat and reconciled into the
specs/BOM/wiring. None touch the safety chain (DEC-003 / DEC-012) or the schedule — this is a
sourcing/packaging reconcile, not a redesign.

1. **Power tree — the 24→3.3V buck is removed.** A **single 24→5V buck** is the only converter: it
   feeds the flow sensor *and* the ESP32, which takes 5V on its 5V/VIN pin and makes its own 3.3V
   on the onboard regulator. The 3V3 pin then sources the logic rail (the WD-trip pull-up, the
   level-shift reference). One fewer module, one fewer thing to misadjust. (The 12V buck was already
   gone under DEC-019; the LED-ring load that needed it left with the panel.)
2. **Pump → SEAFLO 55** (SFDP-055-060-55, 24V, 7.0 GPM open), with a heavy-duty pressure switch and
   anti-vibration mounts, set to ~20 psi. Supersedes the 51 (which superseded the 42). The
   accumulator matters even more against the 55's higher open flow vs. the 1.78 GPM zone.
3. **Flow sensor → Leridian 3/4" NPT** hall unit (2–45 L/min, 253 psi), mounted after the filter.
   It **won't register below 2 L/min**; our zone runs ~1.78 GPM ≈ 6.7 L/min, comfortably above the
   floor. The firmware's seed/default K-factor changes to the Leridian's — **[FW], tracked
   separately** — but field calibration (DEC-015 path) is unchanged and authoritative regardless.
4. **Per-FET clamp → 1.5KE30CA (through-hole, bidirectional)**, not the SMD SMAJ30A. Same job (drain-to-source TVS;
   the internal-rectifier valves can't be clamped by a freewheel diode across the load), through-hole
   to suit the protoboard build. *(As-built 2026-07-04: the **bidirectional 1.5KE30CA** was ordered
   in place of the A and kept — positive clamp identical, and the FET body diode already covers the
   negative direction the A's forward junction would have. No band; either orientation.)*
5. **PSU mounts outside the enclosure** on its own bracket (shaded/vented), not crammed inside the
   sealed box. The open-frame LRS is IP20, so the bracket gives it air; the enclosure then only
   houses the controller/driver/terminals.
6. **Enclosure → opaque grey Boxco P-series** (~170×220×100), UV-stable, vertical mount, glands down,
   DIN rail inside with **lever terminal blocks** + jumper bars for the 24V+/GND rails. Build is
   **hybrid**: ESP32 on a screw-terminal breakout, bare IRLZ44N FETs + a socketed ATtiny85 on an
   ElectroCookie protoboard, ×2 relay modules (pump + safety). The AC master switch (DEC-019) is
   **optional** — without it, the AC breaker / unplug is the kill.

**Why it's safe:** every item is power-packaging or part-substitution. The fail-dry barrier is still
the pump-power gate (DEC-012), armed by the ATtiny (DEC-003); valves still rest closed on raw 24V
(DEC-011); the §17 acceptance checklist is unchanged. The one firmware-touching item (the Leridian
default K) is a constant, gated behind empirical calibration, and ships in its own PR.
**Status:** Decided. Docs/BOM/wiring reconciled in this pass; the default-K firmware change is
tracked as a separate task/PR.

## DEC-021: Diverter returns to plain at run end (SETTLE), not left energized
**Decision:** On normal run completion, `RunController` returns the Dosatron diverter to its plain
rest state at SETTLE (both leg FETs LOW) **when no run is queued** — not left in its last commanded
position. A chained queued run still sets its legs in PREP, so the return runs only when the queue
drains (`qCount_ == 0 && diverterFert()` at SETTLE entry; the `diverterFert()` guard keeps a plain
run's SETTLE travel-free).

**Why:** The prior code left the diverter "as-is" at SETTLE, citing no decision — unlike the DEC-018
history-push immediately adjacent. That contradicted the ratified rest-state contract (DEC-011,
DEC-013, firmware spec §5/§14, §17 acceptance: "unpowered rest = plain water flows, Dosatron
isolated"). Under §6's one-fertigation-run/day policy the consecutive-same-state skip-travel
optimization that motivated "as-is" effectively never fires — the run after the day's fert run is
plain, so the legs differ and travel happens anyway — while the cost was paid daily: pin 18 (fert
leg) held **energized/open** and pin 17 (bypass) held **closed**, two FETs and two valve coils
energized continuously for up to ~24h between runs. The optimization bought nothing and violated
both the rest-state contract and the unpowered-valve power budget (DEC-011).

**Why it's safe / scope:** Not a fail-dry change. With zones NC-closed and the pump off (DEC-012
source gate), an open fert leg is dead-ended upstream of the source — DEC-013's "any diverter state
is at worst a fertigation error, never a flood" holds either way. The `PrepDiverter` skip-travel
guard (§6) is unchanged and still serves the genuine consecutive-same-state case, which now lives
only inside a queue. §6 policy and §15 travel timing are untouched; firmware spec §4 step 7 corrected
from "Diverter legs left as-is" to the queue-aware return. **Surfaced at bench validation (task 6.1a):**
the fert diverter LEDs stayed lit after a run ended.
**Status:** Decided. Implemented in the same PR (core + host tests + spec correction).

## DEC-022: OTA firmware update over Wi-Fi — IDLE/FAULT-gated, run-inhibited, already dual-slot
**Decision:** Add `POST /api/ota` (#126): a firmware `.bin` streams from the phone straight to
`Update.write()` on the existing ESPAsyncWebServer — no external OTA lib. Three guardrails:
(1) **begin-gate** — accepted only from IDLE **or** FAULT with an empty queue (`Api::postOtaBegin`,
core policy, host-tested); (2) **run inhibit** — the accept sets `RunController::setOtaActive(true)`
and `requestRun()` refuses while it's set, so a scheduled run coming due during the 10–30 s upload
can never energize the pump mid-flash (every run funnels through the `IRunSink` seam — scheduler and
API covered in one place); a failed or dropped upload lifts the inhibit, a good one ends in reboot;
(3) **optional shared secret** — `X-OTA-Key` header checked against a build-flag from an untracked
`ota_secret.ini` (the repo is public; a committed secret is not a secret). `/api/status` gains
`build` (a `<sha>[-dirty]-<UTC timestamp>` identity via `tools/fw_build_id.py`; the timestamp makes
it change on every flash, #159) so a flash is observable from the phone.

**Why OTA-from-FAULT is allowed:** a latched fault is pump-off and queue-unwound — the safest state
the system has — and reflash is a legitimate recovery path. The latch is RAM-only and re-derives
from live conditions after reboot, so OTA cannot launder a fault past the resolved-condition gate.

**No repartition (the fact worth recording):** the device already runs the arduino-esp32 default
4 MB table — `app0`/`app1` at 1280 KB each + `otadata`. It was dual-slot all along; the issue's
assumption that OTA required a partition switch (and one final USB flash for it) was wrong, verified
by reading the flashed `partitions.bin`. Current image ≈ 904 KB = 69% of a slot;
`tools/fw_build_id.py` hard-gates the build against slot overflow so growth fails loudly at build
time, not at flash time. The 1408 KB `spiffs` region stays as dead space — reclaiming it isn't worth
a repartition (DEC-002 keeps the SPA in PROGMEM).

**No bootloader rollback (scope honesty):** `esp_ota` mark-valid/rollback is a no-op on the stock
prebuilt arduino-esp32 bootloader (no `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). What protects the
device instead: `Update.end(true)` validates magic + checksum **before** `otadata` flips, so a
truncated/corrupt upload never becomes the boot target — the old slot keeps booting. A
valid-but-broken image is recovered over USB, which remains the recovery path of record; OTA is a
convenience on top, not a replacement. Amends DEC-002's "a UI change requires a reflash" — the
reflash is now wireless.

**Fail-dry through a flash:** nothing new needed. The heartbeat is emitted only in
START_PUMP/RUNNING (§9); OTA is IDLE/FAULT-gated, so the heartbeat is already parked and the ATtiny
relay already de-armed before the first byte is written. The reboot just extends that pump-unpowered
state into the new image's boot. §17 gains the acceptance line: an OTA in progress refuses any run
request until reboot.
**Status:** Decided. Implemented with #126 (core inhibit + host tests, web route, SPA control, build
scripts, spec §10/§17). **Coverage honesty:** the native tests exercise only the core policy gate
(`postOtaBegin` / the `requestRun` inhibit); the chunk-streaming route in `web_server.h` and its SPA
multipart pairing are ESP32-only and untestable on the host — the first flash of the wet-run window
is the end-to-end verification (§17 item).

## DEC-023: Watchdog fault is non-latching — the schedule must survive a trip
**Decision:** `FAULT_WATCHDOG` no longer latches the system. Three coordinated changes:
1. **Trip-line qualification (`TRIP_CONFIRM_MS`, 100 ms):** the trip input (GPIO36, 10k pull-up,
   sampled once per ~10 ms tick) must read asserted CONTINUOUSLY for 100 ms before it counts —
   raw single-sample reads produce nothing. A genuine LOCKOUT holds the line ≥ `HB_TIMEOUT_MS`
   (2 s) by construction, so qualification delays a real verdict by 100 ms and erases glitches.
2. **Confirmed trip mid-run → `RunController::abortRun`:** the current run unwinds through the
   normal StopPump→CloseZone→Settle path and logs `Faulted(Watchdog)`; the QUEUE IS PRESERVED
   and nothing latches. `raiseFault(Watchdog)` is structurally refused, like DEC-014's ValveRest.
3. **Pre-open gate waits instead of killing (`WD_WAIT_MS`, 60 s):** a run reaching the
   PREP_DIVERTER→OPEN_ZONE transition with the line asserted HOLDS (nothing energized) until the
   lockout self-releases (spec'd ≤ 2 s of heartbeat quiet); stuck past 60 s, THAT run alone is
   skipped (logged Faulted) and the queue advances.

**Why (field event, 2026-07-09):** a spurious `FAULT_WATCHDOG` latched at the end of the first of
three noon runs. The old path dropped the queue and blocked every later scheduled run behind a
manual clear — one glitched GPIO read cost a full day of watering. The trip can't have been real:
the ATtiny asserts TRIPPED only on the 30-minute `HARD_MAX_RUNTIME` lockout, and the run was
minutes long. The likely source is switching transients at pump-relay opening landing in the
~21 s run-end tail (~2,000 unqualified samples per run end), any one of which latched.

**Why this loses no safety:** the ATtiny + safety relay are the backstop, and they are untouched —
the relay cuts pump power on its own clock, per run, whatever the ESP32 believes (DEC-003/012).
By the time the ESP32 even reads the trip line, the pump is already de-powered; the ESP32-side
latch was bookkeeping that punished the schedule. Every subsequent run re-arms fresh under its own
30-minute hardware ceiling, so runaway remains bounded with no latch at all. The failure direction
matters: this system's charter is fail dry, but a detector that regularly fails toward
NOT-watering is a worse outcome than the hazard it guards (same reasoning as DEC-014/DEC-015 —
a safety-adjacent detector must not block irrigation on a false positive). Wet-side faults
(`FAULT_UNEXPECTED_FLOW`, `FAULT_NO_FLOW`) keep their latch: they indicate water moving where it
shouldn't, the direction that must still fail stopped.

**Visibility:** every trip still lands in the fault-log ring (`FaultManager::note`) + serial, and
the aborted/skipped run logs `Faulted(Watchdog)` in the run history — a chronic line problem shows
up as a pattern in History, not as silence.
**Status:** Decided (field-driven). Amends §4 step 2, §9, §14, §15, §17. Supersedes the latch
behavior of #48/#49; the DEC-016 host-test discipline carries over (new tests for qualification,
abort-preserves-queue, hold-then-proceed, stuck-line skip).

## DEC-024: Distributed Watering — day-level even-spread irrigation via Scheduler emission (supersedes #136 cycle-and-soak)
**Decision:** Add **Distributed Watering**, a mutually-exclusive alternative to the fixed-time
entry schedule (either the schedule runs, or Distributed Watering runs — never both). The operator
sets a daily **window** (start/end), a **per-zone daily water budget** (minutes), and a **fert
count** (fertigate the first N waterings). The Scheduler derives a run count from a fixed per-run
floor, computes evenly-spaced cycle boundaries across the window, and at each boundary emits one
run per zone (queue depth 3, under `MAX_QUEUE=4`). Each bed soaks between its runs while the other
zones water — **the rotation is the soak**, so no `RunController` sub-state is added.

Fert is quantized to **whole cycles** (each of the first N boundaries fertigates all three zones),
which structurally avoids a per-zone fertilizer imbalance. Config lives in **one packed NVS record
under its own key**, alongside the schedule blob; `ScheduleEntry` is untouched and `SCHEMA_VER`
stays 1 (DEC-008 additive rule). **Zero diff** to the watchdog, heartbeat, and pump-gate paths.

**Firmware run floor + cap (agronomic, operator research 2026-07-16):** each run is **≥ 7 min**
(hard firmware floor) and **≤ 6 runs/day** — "distributed constancy," not micropulse. Even spacing
pins low run counts to the window edges (2 runs → one near start, one near end); the operator tunes
placement by narrowing the window (the bookend pattern is intended, not a bug).

**Daily-water guideline (softened):** the plan preview shows projected gal/day against a
**summer guideline band (110–140)**, hardcoded for now and rendered as an **informational note,
never a block** — a fixed band is wrong most of the year in both directions, so it must not cry
wolf on a correct early-spring setting. Making the band seasonal is deferred to #139. The **only
hard stop** is a safety ceiling at **25 % of catchment/day** (~632 gal), which blocks the save.

**Why:** infiltration on compacted tunnel beds without a `Soak` sub-state, without touching the
ATtiny arm/de-arm path, and without the cumulative-vs-per-pulse `swMaxRuntimeSec` ambiguity a
`RunController`-internal soak would force. Distributed Watering is a tunnel-level *mode*, not a list
of entries: 8 cycles × 3 zones = 24 runs exceeds `MAX_ENTRIES=16`, so the entry list cannot express
it at all — one config record is forced, not merely preferred.

**Alternatives rejected:**
- **RunController-internal `Soak` state (#136 cycle-and-soak):** superseded. Redundant once the
  rotation supplies the gap; would add a state, hold a zone FET energized across every soak, and
  require new heartbeat/flow-check reasoning for no gain. The operator's own agronomic research
  ("distributed constancy, not a micropulse generator; hard-floor 7 min") independently rejects it.
- **Pulse spec on `ScheduleEntry`:** impossible (24 > `MAX_ENTRIES=16`) and would force a format
  bump on a live packed record.
- **Fert pinned to window start / global minute budget:** a budget that isn't a whole multiple of
  the cycle gives some zones fert others don't get, every day. Whole-cycle quantization fixes it.
- **Hard-blocking the 110–140 band:** rejected — a fixed peak-season band nags correct spring/fall
  settings. Guideline is informational; the catchment ceiling is the only block.

**Consequences / envelope:** IDLE entries rise ~3/day → 4–8/day; pump starts ~9/day → ~24/day
(duty confirmed by operator). DEC-014 `ValveRestMonitor` fires more often (log-only, but churns the
fault ring — another reason E2 must be fixed first). Worst-case daily draw is bounded by the
25 %-catchment block. **The day becomes a single point of failure:** any *latching* fault mid-window
costs the rest of the day's water, where the 3-block schedule banked early water — this is the real
cost of the trade and the reason the E2 gate below is a hard prerequisite.

**E2 latch preserved (consistency with DEC-023):** `FAULT_UNEXPECTED_FLOW` keeps its latch — a
stuck valve with a live pump must still fail stopped, and DEC-023 exempted only the watchdog
because the relay was already its backstop; nothing backstops E2. But today's idle-flow detector
false-positives on normal draindown (threshold ~0.36 GPM inside a decay tail the 60 s `DrainGate`
cap doesn't outlast), which at 4–8 IDLE entries/day would latch out ~a day of water essentially
every day. **Distributed Watering is therefore gated on the E2 discriminator fix** (make the
detector discriminate persistent flow from draindown decay — keeping the latch and the fail-dry
direction, killing only the false positive). See the E2 tracking issue + #138.

**Status:** Decided (design; architect-reviewed 2026-07-16, mock approved). **Blocked on the E2
discriminator** before any watering ships. Adds §-TBD to the firmware spec at build time. Milestones:
E2 discriminator + #138 (gate) → Distributed Watering core (Scheduler emission + config + fert
quantization + validation) → SPA editor. Seasonal guideline is #139 (deferred).
