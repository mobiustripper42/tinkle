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
**Tradeoff:** Bench-tunable constants (`PULSE_MS`, `DIVERTER_TRAVEL_MS`, flow
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
  Tunnel's Z1/Z2 — under build-for-three: wired now (3rd latching valve + H-bridge, see
  DEC-007), plumbed when that line goes in.
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
fail-dry chain (sw ceiling → ATtiny → NC master) is untouched. The web `/api/fault/clear`
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
log), and both-low means **no spurious valve pulse at boot** (fail-dry). This mirrors
the existing master-FET gate-pulldown boot note — an established pattern here.
**Rejected:** GPIO0 (pulldown → download mode at boot), GPIO5 (must be HIGH at boot,
pulldown fights it), GPIO2 (works, but its onboard LED flickers on every Z3 actuation),
and an I/O expander (adds an I2C/SPI dependency, a part, and a failure mode to dodge a
cosmetic boot-log — against build-for-three/populate-one).
**Constraint:** **Nothing may pull GPIO12 high** — no external pull-up, no scope probe
with a pull, nothing. Documented in the wiring doc §D so the next person doesn't brick
boot without knowing why. (Supersedes the wiring doc's prior "no Z3 pins allocated.")

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
