# Tiller proposal — Armed-rail sense: the safety relay's missing witness

> **Draft for review — Eric is the gate.** One overnight idea for tinkle, worked
> through Tiller's Innovator + Skeptic/Visionary/Architect panel. No branch of the
> mechanism is touched; nothing merges without you.

---

## The idea

Every failure mode in tinkle has a witness — except the one that matters most.

- A valve that won't close → **DEC-014** `ValveRestMonitor` flags it.
- No water when there should be, or water when there shouldn't → **§7** flow checks.
- The ESP32 hangs → the **ATtiny** trips and the trip line tells the ESP32.

But the **safety relay itself** — the normally-open contact the ATtiny arms to feed
24 V to the pump, the actual fail-dry barrier (DEC-012) — is watched by **nothing**.
Your own v1.4 failure-mode table says it out loud: *"a welded safety relay is the
single hardware point that defeats it, with the software flow check behind that."*
And that backstop is thinner than it reads: `FAULT_UNEXPECTED_FLOW` only fires if
water actually **moves** at idle, which needs a zone *also* stuck open. A welded
relay at idle with the NC zones resting closed passes no water and is **completely
invisible** — a silent, armed gun that only becomes observable the day a *second*
fault (a stuck zone) arrives. That two-fault coincidence is exactly the flood
fail-dry exists to prevent.

**Give the relay a witness.** A resistor divider taps the *gated pump-side* of the
safety relay (the `24V-armed` rail) down to < 3.3 V into a spare ADC-capable
input-only pin (GPIO34, freed by DEC-019, banked for future zones). The ESP32 reads
whether the pump-power rail is live. **Read-only, forever** — it never commands the
relay, so the deliberate ATtiny-independence of the chain (DEC-003/DEC-012) is
untouched. This is the safety-side twin of DEC-014: that one catches a valve that
won't *close*; this one catches a relay that won't *open*.

The whole feature is one four-quadrant check, run each tick beside
`FlowFaultDetector`:

| Commanded run? | Rail sensed | Meaning | Response |
|---|---|---|---|
| yes | live | correct — arm took | ok |
| yes | dead | arm didn't take (relay/coil/fuse/supply) — *this run won't deliver water* | `note()`, log-only |
| no | dead | correct fail-dry rest | ok |
| **no** | **live** | **welded relay / stuck-armed — source hot at idle** | **latch, run-inhibit, scream** |

Only the bottom-left quadrant is dangerous, and only it latches. When it fires, the
ESP32 does something real — not theater: it **holds its own pump relay (GPIO22) open
and refuses to start runs**. The pump sits behind *two* series contacts (safety relay
+ pump-enable relay); one welding closed means you've lost one of two barriers, not
that water is flowing now. Refusing to command the second barrier closed, and holding
the fault until a human inspects and clears it, is the honest, in-bounds response.

## Why it's worth it — and why now

- **It closes the one blind spot on the product's core promise.** tinkle waters a
  working farm unattended for a season. The failure that voids "fail dry" is a welded
  safety relay, and today it's undetectable until it's too late. This makes the
  single defeat of fail-dry a *visible, logged, service-actionable* state — caught
  while it's still a harmless single fault, before the second one lands.
- **It's the phase you're in.** #96 (watchdog + safety relay bring-up) was the noisy,
  painful bench step, and the Step-8 check is *"meter `24V-armed`; start a run, it
  reads 24 V; pull pin 4, it drops to 0 in ~2 s."* Right now the human is the
  instrument. With the sense line the firmware reads that itself and the phone walks
  it — no multimeter on the one signal that matters most. This lands in the bench work
  that's live this week, not months out.
- **The pattern is already blessed.** DEC-014 is exactly this shape: a read-only
  detector on safety-*adjacent* state, non-actuating, routed through `FaultManager`,
  explicitly *not credited* as a safety layer. This is the second instance of a line
  you already drew — pointed at the component your docs already flagged.

## Why it was hiding

The whole fail-dry architecture is built to keep the ESP32 **out** of the safety
loop: the ATtiny + relay is independent hardware precisely so a hung or confused
ESP32 can't keep the pump armed (DEC-003/DEC-012, load-bearing). That instinct is
correct — and it governs the *command* path. But it got silently over-generalized to
the *observation* path: the ESP32 was wired to the relay domain only as a commander
(`PUMP_RELAY` out) and to the ATtiny only via the trip line, **never to the armed
rail as an observer.** The failure-mode table even names the welded relay as the
single defeat, then *shrugs* — leans on the flow check — because adding an ESP32 wire
to the armed rail *felt* like a category violation of the independence principle.
It isn't: observing a voltage is not commanding a relay (you already read the arming
domain once, via `WD_TRIPPED_IN`). Nobody weighed "add a read-only sense line and the
relay finally gets a self-test" — it was never separated from the command-path
prohibition it got bundled with. That's the tell it's a real seam: the one component
carrying the core guarantee is the one component with no witness, and the reason is a
principle mis-applied one category too wide.

---

## Build handoff

Parts-gated like the rest of Phase 6 (the divider + tap is a board addition), but the
**core detector + host tests land today**, off-hardware, in the load-bearing tier
(DEC-004). Ship the MVP; the analog arc below is a named fast-follow, not this PR.

### Approach — the clean shape (the only right one)

Mirror `FlowFaultDetector` / `ValveRestMonitor` exactly: **hardware read in the shim,
pure decision in core, route through `FaultManager`, never actuate.**

- **`src/esp32` shim** owns everything hardware-coupled: the ADC1 read on GPIO34,
  divider scaling, attenuation, hysteresis + debounce, and the raw warmup validity.
  It emits **one typed value**: `RailState { LIVE, DEAD, UNKNOWN }`. No decision logic.
- **`src/core` detector `ArmSenseMonitor`** is a **pure function of
  `(railState, commandedRunState, now)`** per tick → verdict. Zero ADC in core.
  Host-tested with an injected fake `RailState`, exactly as fake GPIO / fake clock
  work today. This is where the four-quadrant table, the warmup gate, and the
  trailing-edge deadband get full coverage on the fake clock.
- The detector **pushes** its verdict; it never touches a relay. `RunController`
  stays the sole actuator commander.

### Two conditions, two severities (get this right)

- **OFF/live (welded relay / stuck-armed): LATCH** via `RunController::raiseFault`
  (new `FAULT_ARMED_STUCK`). The meaningful safe-state is **run-inhibit** — a small
  addition to how `RunController` treats *this* fault: decline to command any run
  while the interlock is known-defeated, and hold the fault (resolved-condition clear
  gate) until a human clears it. Do **not** claim pump de-energize as the response —
  the ESP32 can't un-weld a relay; run-inhibit + the independent pump-enable relay
  held open is the real, in-bounds action.
- **ON/dead (arm didn't take): `note()` only**, log-only ring entry
  (`FAULT_ARM_NOT_CONFIRMED`, self-healing, DEC-014 style). It largely overlaps
  `FAULT_NO_FLOW` (no arm → no pump → no flow), so its only marginal value is
  *disambiguating* "relay/coil/supply didn't arm" from "pump ran, no water
  (clog/dry)." Build it second, or skip it for the Phase-6 MVP. Never let this
  quadrant halt irrigation — that's the DEC-015 anti-pattern (a flaky divider starving
  the farm).

### The self-covering blind spot (must ship together)

A broken divider (cracked solder) reads **~0 V = "rail dead"** — which reads as
*safe*, so a broken sense **masks the very weld it exists to catch.** Mitigation is
elegant and non-optional: **pair weld-detect (idle-hot) with arm-confirm (run-live).**
A sense that *never* reads LIVE under a commanded run is self-diagnosing as broken →
warn. The two halves cover each other's failure direction. This is why the ON/dead
quadrant earns its keep even though it overlaps the flow check.

### File-by-file

- **`src/core/arm_sense_monitor.{h,cpp}`** — new. Pure detector. Inputs
  `(RailState, RunState commanded, uint32_t nowMs)`; outputs a verdict enum
  (`None | ArmedStuck | ArmNotConfirmed`). Holds the warmup gate + trailing-edge
  settle timers on the injected clock. No hardware types.
- **`src/esp32/arm_sense.h`** — new shim. `analogRead(GPIO34)` → divider scale →
  hysteresis/debounce → `RailState`. Under `TINKLE_SIM`, **force `RailState` to the
  healthy correlation** (`LIVE` iff a run is commanded), same reason the trip read is
  forced clear — Wokwi floats the unconnected input and a floating ADC read is
  garbage. Keep an injection hook so the welded case is still *demonstrable* in sim.
- **`src/core/fault_manager.*`** — add `FAULT_ARMED_STUCK` (latching) +
  `FAULT_ARM_NOT_CONFIRMED` (note-only) codes beside `FAULT_VALVE_REST`.
- **`src/core/run_controller.*`** — accept the pushed verdict; on `ArmedStuck`,
  `raiseFault` + set the **run-inhibit** flag that `requestRun()` already knows how to
  honor (same shape as the FAULT gate).
- **`src/esp32/main.cpp`** — each tick, read the shim → push `RailState` into
  `ArmSenseMonitor::update(...)` → route the verdict (mirrors the existing
  `FlowFaultDetector` wiring). Add a `static_assert`/comment fencing the tap-point
  constraint below.
- **`src/esp32/pins.h`** — claim `ARM_SENSE_ADC = 34` (input-only, ADC1) from the
  DEC-019 freed bank; note "no internal pull — external divider provides DEAD≈0."
- **`src/core/*` §15 constants** — `ARM_LIVE_COUNTS` / `ARM_DEAD_COUNTS` (hysteresis
  pair, ADC counts), `ARM_DEBOUNCE`, `ARM_WARMUP_MS` (post-boot cap-charge blanking),
  `ARM_SETTLE_MS` (trailing-edge deadband). **Compile-time bench-confirmed seeds
  first** — no NVS. If field-tuning later proves necessary they become additive
  per-key-read-with-default entries (DEC-008 — no `schema_ver` bump).
- **`/api/status` + SPA** — additive `armRail` state + weld flag (display-only,
  Faults screen renders it beside `valveRestFlags`). Read-only; no fail-dry role.
- **Boot-log line (folds in candidate C):** once `RailState` becomes valid past
  warmup, write a first-valid-window attestation to the persisted log — *"rail
  confirmed DEAD at idle → interlock de-energized."* Gated on validity, **not** fired
  at boot (boot is the worst-timed read — cap-charge transient).
- **Docs:** new **DEC-022** (below); rewrite the v1.4 failure-mode table entry from
  *"welded relay — accepted/invisible"* → *"detected/logged (DEC-022)"*; add a §17
  acceptance item; add the divider + tap to `docs/tinkle_buildup.md` and the netlist.

### Gotchas / risks (all load-bearing)

1. **Tap the gated pump-side of the relay, NEVER the ATtiny arm/enable control node.**
   A sense-side fault on the gated *output* can only affect a node already downstream
   of the interlock; a sense-side fault on the *control* line could arm the pump or
   defeat the ATtiny — turning this feature into the exact failure it detects. This is
   the line between blessed observability and breaking fail-dry. **Bench go/no-go.**
2. **Shared ground is a hard prerequisite.** The 24 V rail ground must be common with
   the ESP32 ADC ground (it is, per the build-up netlist — one common GND). If the 24 V
   section were ever isolated, a resistive divider is meaningless *and* unsafe — you'd
   need isolated sense. Confirm on the bench before committing the divider. **Go/no-go.**
3. **Must not load or pull the rail.** High-impedance divider (top resistor in the
   hundreds of kΩ), **series R + clamp on the ADC pin** so a shorted top resistor can't
   put ~24 V on GPIO34, plus a small filter cap. Input-only pins physically can't
   backfeed the rail — a feature here.
4. **Post-boot cap-charge (~1 min):** the rail's bulk capacitance makes the boot
   reading the least trustworthy moment. The warmup gate must hold off `raiseFault`
   until settled, or you latch a false weld every boot.
5. **Arm/disarm trailing edges:** relay dropout + cap discharge means "live while
   idle" is briefly true after every legitimate run. The settle deadband must expire
   before the weld rule re-arms — same timing-logic class as `ValveRestMonitor`.
6. **ADC honesty:** ESP32 ADC1 is nonlinear/noisy near the rails; 5 % resistors +
   24 V ripple mean no knife-edge threshold. Hysteresis + debounce + multisample;
   characterize actual counts at true-live / true-dead on the bench before seeding §15.
   **Do not** claim milliohm contact-resistance trending — an idle rail carries no load
   current, so no IR drop; that's a fantasy on a single single-ended ESP32 tap.
7. **Never load-bearing.** The ATtiny still disarms on lost heartbeat, independently
   and mute. This is a second pair of eyes, not a second brain — keep DEC-014's
   sentence verbatim: *the detector is explicitly not credited as a safety layer.*

### Done when

- `pio test -e native` green: four-quadrant verdicts; welded-latch; arm-not-confirmed
  note; warmup gate suppresses the boot transient; trailing-edge deadband suppresses
  the post-run edge; broken-sense (never-live-under-command) self-diagnoses; run-inhibit
  blocks `requestRun()` while latched and the resolved-condition gate clears it.
- `esp32` / `esp32_sim` / `attiny85` build; SPA under the 50 KB gzip gate.
- An `esp32_sim` session: healthy runs show `armRail: LIVE` during RUNNING, `DEAD` at
  idle; the injected welded case latches `FAULT_ARMED_STUCK`, inhibits runs, and
  surfaces on the Faults screen.
- **Zero diff** to the ATtiny sketch, the watchdog path, `HEARTBEAT_OUT`, or the
  pump-power gate. The sense line only reads.
- DEC-022 written; failure-mode table + §17 + build-up doc + netlist updated.

### The bold arc (named fast-follows — NOT this PR)

Same one wire, more firmware, once the MVP earns its keep:
- **Arm-latency** — time from first heartbeat edge to rail-live; a health trend on the
  coil + contacts. Quantifies the phone-guided bring-up.
- **Rail-sag under load** — sample during RUNNING with the SEAFLO 55 drawing current;
  catch inrush dip / sustained sag (undersized supply, failing bearing, drooping
  catchment). Gross sag only — honest for this ADC.
- **Per-run electrical vitals** — fold min-rail-under-load + arm-latency into the
  DEC-018 `RunLog` (a couple of packed fields, existing storage pattern); an
  "electrical vitals" `/api/status` surface for bench *and* field diagnostics.
- **Second raw-24 V tap** → differential across the relay contact; distinguishes
  "relay disarmed" from "supply dead," and seams toward the DEC-017 low-tank story.
  V2 — gated behind "do we actually need it."

### DEC-022 (draft)

> **DEC-022 — Armed-rail sense (`ArmSenseMonitor`).** Read-only resistor-divider tap
> from the *gated pump-side* of the safety relay into ADC1 (GPIO34), scaled/debounced
> in the esp32 shim to `RailState`; a pure `ArmSenseMonitor` in core decides the
> fault. Extends the read-only safety-observability pattern (DEC-014, DEC-018).
> Rail-live-while-idle (welded relay / stuck-armed) **latches** via `raiseFault` with
> **run-inhibit** as the safe-state (the ESP32 cannot un-weld the relay but refuses to
> command runs into a defeated interlock and holds its own pump-enable relay open);
> arm-didn't-take is `note()`-only and overlaps `FlowFaultDetector`. The tap is
> galvanically downstream of the interlock, high-impedance, ADC-clamped — **never** the
> ATtiny arm-control node — preserving DEC-003/012 independence: no sense-side fault
> may load, backfeed, or arm the rail. `TINKLE_SIM` forces `RailState` to the healthy
> correlation in the shim (Wokwi floats the input); the welded case is host-tested via
> injected fake state. Thresholds/debounce/warmup/settle are §15 bench-confirmed
> constants; any later tuning keys are additive NVS (no `schema_ver` bump, DEC-008).
> The detector is explicitly **not** credited as a safety layer — the ATtiny + relay
> remain the barrier, independent and mute.

### Kickoff (paste-ready for a CC session on the tinkle repo)

> Implement DEC-022 armed-rail sense per `docs/proposals/armed-rail-sense.md` — the
> Phase-6 MVP only (defer the analog arm-latency/rail-sag fast-follows). Build the
> pure `src/core/arm_sense_monitor.{h,cpp}` detector first, TDD in the native tier:
> four-quadrant verdicts, welded-latch + run-inhibit via `RunController`, arm-not-
> confirmed `note()`, warmup gate, trailing-edge settle deadband, broken-sense
> self-diagnosis. Then the `src/esp32/arm_sense.h` shim (ADC1 GPIO34 → `RailState`,
> `TINKLE_SIM` forces the healthy correlation) and wire it into `main.cpp` beside
> `FlowFaultDetector`. Add the `FaultManager` codes, `pins.h` `ARM_SENSE_ADC=34`, the
> §15 constants, the `/api/status` + SPA display flag, and the boot-attestation log
> line. Keep it read-only — zero diff to the ATtiny, the watchdog path, or the pump
> gate. Write DEC-022 and update the v1.4 failure-mode table, firmware §17, the
> build-up doc, and the netlist for the divider + gated-pump-side tap. Bench work
> (divider, shared-ground + tap-point go/no-go) is parts-gated — land the core +
> tests + docs now. `pio test -e native` and the esp32 builds green before `/kill-this`.

---

🤖 Generated overnight by Tiller. One idea, no merge — your call.
