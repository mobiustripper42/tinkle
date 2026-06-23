# Volume-dosing — a run delivers *gallons*, not minutes

**Tiller overnight idea — tinkle.** Draft for review; you're the gate. Pitch +
execute-ready build handoff below.

---

## The idea

A run is a stopwatch with a valve bolted on: open the zone, count seconds, close it.
The flow meter is a passive witness — it integrates `gallons()` per run and the history
log records the number *after the fact*. That's backwards. You already have a meter and a
calibrated `K`. The controller should be **dosing water, not spending time**.

Add a second exit to `RUNNING`: a run can target a **volume**. The controller opens the
zone, integrates flow toward a gallon target, and closes on `gallons ≥ target`. **Time
stays the safety envelope, never the goal** — `swMaxRuntimeSec` and the ATtiny
`HARD_MAX_RUNTIME` remain the unconditional hard cap, so a clogged or stuck-meter zone
still stops exactly as it does today. When flow is muted (DEC-015) or `K` is uncalibrated,
Volume mode is inert and the run reverts to pure time — and the run says so in the log.

Mechanically it rides the seam you already built. `RunController` is deliberately decoupled
from `FlowMonitor` (its header: gallons live in FlowMonitor "to preserve the
RunController↔flow decoupling"). It already takes *pushed* state — `raiseFault(code)`,
`setWatchdogTripped(bool)`. Volume-dosing is one more pushed input: `setRunGallons(float)`,
the twin of `setWatchdogTripped`. The controller stays the sole actuator commander and the
only place the exit condition lives. No new hardware, no network, the safety chain
untouched.

The quiet payoff: **gallons becomes the one unit the controller speaks.** It's the
*setpoint* (this run), and — because every dosed run records how long the target took — the
*diagnostic* (a zone that used to hit 12 gal in 8 min now needs 14: emitter fouling or
sagging supply), and the *currency* (Σ gallons against the finite ~2530-gal catchment).
One number — the only number a drip system was ever really about: how much water hit the
root zone.

## Why it's worth it — and why *now*

**It's the half of closed-loop you can ship today.** The SPEC files closed-loop as V2 and
frames it entirely as *soil-moisture-driven* — needs Soundings, needs valves+sensors to
"earn a season." But that bundles two different loops under one label. There's a
**supply-side** closed loop (meter what you deliver, stop at the delivered volume) that
needs only the meter already on the board, and a **demand-side** loop (water to what the
soil needs) that needs Soundings. Volume-dosing is a *real feedback loop in V1* — it splits
the V2 boundary along a seam that was always there.

**It self-corrects the exact failure the flow checks only detect.** A partially clogged
emitter under time-dosing silently *under*-waters — well above the ≈0 no-flow floor, so it
passes clean (this is the gap PR #65 exists to *flag*). Under volume-dosing the same zone
just runs a little longer to hit its gallons — bounded by the time cap, then it completes or
faults. Dosing *acts* on degradation where #65 *alerts* on it; they're complementary, not
the same idea.

**The schema is wet cement this week.** The DEC-018 `RunLog` record (#69) is being designed
right now and isn't merged yet. Its flags byte (`fert | result | clockWasValid`) has spare
bits. Reserving two of them now — `dosedByVolume`, `fellBackToTime` — is free; amending a
shipped, NVS-persisted packed record later is a migration. Land the bits while the cement is
wet.

## Why you haven't already

The closed-loop slot in your head is labeled "soil moisture" and filed under V2 behind
Soundings — and the docs reinforce it: SPEC says "V1 is time + duration only," every
RETROSPECTIVE and DEC treats closed-loop as the sensor-driven thing. That's a **category
error, not a deliberate choice.** Nowhere did you weigh dose-by-gallons and reject it — it
was never separated out from the soil-moisture loop it got bundled with. The tell that it's
a real seam and not scope creep: the plumbing is *already there* — `FlowMonitor.gallons()`
integrates per run and re-baselines each run; `main` already logs the per-run volume on the
RUNNING edge. Every piece exists except the one line that closes the loop.

---

## Build handoff

Scale: one coherent feature, host-testable in `src/core` before any of it touches hardware.
The headline is **Phase A**; #2/#3 below are named fast-follows, deliberately *not* bundled.

### Approach + key decisions

- **Push, don't pull.** `RunController` gains `setRunGallons(float gallonsThisRun)`, called
  every tick by `main` from `FlowMonitor.gallons()` — identical IoC shape to
  `setWatchdogTripped`. The controller never includes or pulls `FlowMonitor`; the decoupling
  is preserved by construction.
- **OR-form exit, time is the hard cap.** `RUNNING` exits when
  `elapsed ≥ effectiveDurationMs() OR (mode == Volume && armed && gallons ≥ target)`.
  `effectiveDurationMs() = min(durationSec, swMaxRuntimeSec)` stays the outer bound
  *unconditionally*; volume is only ever an **early** exit, never an extension. A 500-gal
  target on a clogged zone still stops on time and the pump-power gate. This is the
  fail-dry boundary and it must not move.
- **Arm volume mode at run start, latch it.** Volume mode requires a calibrated `K` *and*
  unmuted flow (DEC-015) at the RUNNING edge. Capture that into a per-run `volumeArmed_`
  bool. If unarmed, run as Time and set `fellBackToTime`. Don't re-check mid-run — a sensor
  muted mid-run is a separate fault path; the armed run already has its time cap.
- **No confident lies (the Skeptic's catch).** A run that *intended* volume but ran on time
  must be visibly distinct in the History log from both a true dosed run and a plain timed
  run — hence the two flag bits. "Target volume reached" and "ran out the clock because the
  meter was muted" must never render identically.
- **Don't spend reserved byte 9 (the Architect's trap).** The volume target is typed payload
  (a u16 centigallon target + a mode bit), not a one-bit flag. `RunRequest` is in-memory —
  add fields freely, zero NVS cost. `ScheduleEntry`'s 10-byte NVS pack is the only schema
  question — see the scope fork.

### File-by-file

**Core control path (the whole headline can be host-tested here):**

- `src/core/run_controller.h` / `.cpp`
  - `RunRequest`: add `enum class RunMode : uint8_t { Time, Volume }` field `mode = Time`
    and `uint16_t targetCentigal = 0`. In-memory only.
  - Add `void setRunGallons(float g) { runGallons_ = g; }` and a private `float runGallons_`
    + `bool volumeArmed_`.
  - In `enter(RunState::Running, ...)`: latch `volumeArmed_ = (current_.mode == Volume &&
    flowUsable_)`, where `flowUsable_` is a pushed bool (`setFlowUsable(bool)` — K calibrated
    && not muted) fed from `main`. If `mode==Volume && !volumeArmed_`, the run proceeds as
    Time and the SETTLE push marks `fellBackToTime`.
  - In `tick()`'s RUNNING branch: add the OR clause. Keep the existing duration check first
    so time always wins a tie.
  - `RunSummary`: add `bool dosedByVolume`, `bool fellBackToTime` so the SETTLE log push
    carries them. (Gallons still come from FlowMonitor via `main`, unchanged.)

- `src/esp32/main.cpp`
  - Each loop tick during/around a run: `run.setRunGallons(flow.gallons())` and
    `run.setFlowUsable(flow.k() > 0 && !flowDetector.muted())`. You already compute gallons
    on the RUNNING edge; this is one more push per tick.

**History record (coordinate with in-flight #69 — do this *before* RunLog lands):**

- The DEC-018 packed record's `flags u8` currently allocates `fert | result(2b) |
  clockWasValid` (~4 bits used). Claim two spare bits: `dosedByVolume`, `fellBackToTime`.
  Additive within the existing byte — **no record-width change, no `schema_ver` bump** on the
  history ring. This is the cheap half of "why now."

**Schedule entry — the one real schema decision (pick one):**

- **Full (recommended):** `ScheduleEntry` gains `mode` + `targetCentigal`. The 10-byte pack
  widens; that's a *transforming* migration (packed-array stride changes), so bump the
  schedule blob's version and migrate: old 10-byte entries read as `mode=Time`. This is the
  honest move and unlocks **scheduled** dosing — which is where the value is, since the farm
  runs on the schedule, not the manual button. Do **not** smuggle it into byte 9.
- **Scope cut (smaller diff):** ship volume-dosing on `RunRequest`/`POST /api/run` (manual +
  API) only; leave `ScheduleEntry` untouched and defer scheduled targets to the schema bump.
  Clean cut, not a hack — but manual-only dosing is closer to a demo than the feature.

**API + SPA (thin clients over the core):**

- `src/core/api.{h,cpp}`: `POST /api/run` accepts optional `{mode, targetGallons}`;
  validate `targetGallons` against the cal sanity floor (≥ 0.25 gal) and a sane ceiling.
  `GET /api/status`: surface `gallons / target` and `dosedByVolume`/`fellBackToTime` for the
  live + last-run view. If you took the Full fork, `GET`/`POST /api/schedule` carry
  `mode`+`target` per entry. Host-test the JSON shapes as the existing `Api` tests do.
- `web/index.html`: Manual-run screen gets a **by time / by volume** toggle + a gallons
  field (defaults pre-filled). Status shows "8.3 / 12.0 gal" during a dosed run. History
  shows a dosed-vs-timed badge and a "fell back to time" marker. Schedule editor gains the
  same toggle under the Full fork. Stay inside the <50 KB gzip gate.

**Docs:**

- `docs/DECISIONS.md`: **DEC-020** (next free — DEC-019 is the phone-only pivot; the panel's
  draft mis-numbered this). Record the OR-exit, the pushed-gallons seam, the unconditional
  time cap, the arm-at-start/degrade-to-time rule, and the byte-9 prohibition.
- `docs/tinkle_firmware_spec.md` §4 (RUNNING exit), §7 (the dosing read), §15 (any new
  constant, e.g. a default per-zone target). `docs/SPEC.md`: note V1 now has a *supply-side*
  closed loop; the soil-moisture loop is still V2.

### Gotchas / risks

- **The time cap is now load-bearing for dosing too.** Treat any future "smarter cap" as a
  fail-dry change. The ATtiny `HARD_MAX_RUNTIME` and `swMaxRuntimeSec` are the only things
  between "dose 12 gal" and "water until the meter lies forever."
- **Single common-line K, datasheet seed.** One `K` for all zones (calibration note), and
  DEC-015 exists precisely because the sensor can lie. A drifted K means a dosed run delivers
  the *wrong* volume while the log says "target reached." Mitigation is the
  `fellBackToTime`/`dosedByVolume` honesty bits + showing live gallons; don't paper over an
  uncalibrated sensor with a confident number.
- **Fert interaction.** Fertigation timing is a function of *flow through the Dosatron*;
  volume-dosing changes run length, so a fert run's injected dose tracks delivered gallons —
  arguably *more* correct than time-based, but call it out in the DEC so it's a decision, not
  a surprise.
- **Tie-break.** Check duration before volume in `tick()` so an exactly-simultaneous
  boundary resolves to the safe (time) exit.

### Done when

- `pio test -e native` green, covering: volume exit fires at target; **time cap wins** on a
  clogged/under-delivering zone (gallons never reaches target → stops on `effectiveDurationMs`);
  muted flow → runs as Time + `fellBackToTime` set; uncalibrated K (`k ≤ 0`) → same; a clean
  dosed run logs `gallons ≈ target` and `dosedByVolume`; (Full fork) old 10-byte schedule
  blob migrates to `mode=Time`.
- `pio run -e esp32` builds; SPA under the 50 KB gzip gate.
- An `esp32_sim` Wokwi session: a volume-targeted run closes early at target with the meter
  fed; the same run with flow muted runs full duration and the History row shows the fallback
  badge.
- **Zero diff** to the watchdog path, the pump-power gate, `effectiveDurationMs()`'s role as
  the hard cap, and `FlowFaultDetector`. The fail-dry chain is re-verified, not modified.

### Kickoff line (paste-ready for a CC session on the tinkle repo)

> Implement volume-dosing (DEC-020): a run can exit RUNNING on delivered gallons, not just
> elapsed time. Add `mode`+`targetCentigal` to `RunRequest`, push live gallons into
> `RunController` via `setRunGallons()` (mirroring `setWatchdogTripped`, preserving the
> flow↔controller decoupling), and add the OR-exit
> `elapsed ≥ effectiveDurationMs() OR (mode==Volume && armed && gallons ≥ target)` with
> time/`swMaxRuntimeSec`/ATtiny `HARD_MAX_RUNTIME` as the unconditional cap. Arm Volume mode
> at the RUNNING edge only if K is calibrated and flow isn't muted (DEC-015), else run as Time
> and set a `fellBackToTime` flag. Reserve two spare bits in the DEC-018 RunLog flags byte
> (`dosedByVolume`, `fellBackToTime`) before #69 lands — do NOT touch ScheduleEntry's reserved
> byte 9. Start with the `src/core` change + native tests (volume exit, time-cap-wins-on-clog,
> mute/uncal fallback+flag, clean-dose logs target); then the API/SPA toggle. Decide up front:
> full (bump the ScheduleEntry NVS schema, migrate old entries to Time, scheduled dosing) vs.
> scope-cut (API/manual runs only, defer scheduled targets). Write DEC-020 and update firmware
> spec §4/§7/§15.

---

## Named fast-follows (not part of this PR)

- **Water-spend ledger (later, display-first).** Sum per-run centigallons from the DEC-018
  ring into a daily/weekly total against the ~2530-gal catchment; surface on Status/History.
  **Read-and-warn only** — a soft cap that *blocks* runs is a second actuation authority that
  doesn't know the real tank level (that's the @architect-gated DEC-017 lockout, deferred for
  a reason). If a cap is ever built, it lives at the Scheduler/`requestRun` admission layer
  for *auto* runs only, never inside `RunController`, on a path strictly separate from any
  fail-dry stop. Would be DEC-021.
- **Duration-as-health (later, read-only).** Once dosed runs accrue, per-zone
  time-to-target-volume is a clog trend — a pure read over the history ring, no new control
  authority. It and PR #65 read the same pulse stream two ways (integrated-time vs.
  instantaneous-shape); they should share the one DEC-018 schema, not grow a second. The
  `dosedByVolume` bit above is exactly what makes a record trendable. Would be DEC-022;
  revisit ring depth (32 / N zones) only if the trend proves too shallow.

---

🤖 Generated overnight by Tiller. One idea, no merge — your call.
