# Tiller proposal — Per-zone flow-signature health (the flow sensor as an instrument, not a tripwire)

> Draft for review. Not on the plan. One idea, handed off to build in one pass.
> Status: proposal — Eric is the gate.

---

## The pitch

### The idea

Tinkle's flow subsystem is, today, a **smoke detector**: the two checks it runs are
both binary tripwires. `FAULT_NO_FLOW` fires when `rateGPM ≈ 0` during a run;
`FAULT_UNEXPECTED_FLOW` fires when pulses accumulate while idle. Fire / no-fire.
Everything in between — the whole continuous middle of the flow signal — is measured,
logged per run, and then **thrown away**.

The proposal: keep a **per-zone learned band of healthy steady-state GPM**, updated by
an EWMA over clean runs and persisted in NVS, and classify each run's flow against its
own zone's band. One small core module, ticked each loop, returns a verdict the caller
routes to `FaultManager::note()` — exactly the non-latching, self-healing,
`flaggedMask()`-to-`/api/status` shape `ValveRestMonitor` (DEC-014) already established.
No new hardware. It reads the *same* hall-sensor stream you already have.

That single mechanism catches three things the binary checks structurally cannot:

1. **Partial clog / emitter fouling → silent under-watering.** A kinked AquaTraxx line
   or fouled emitters drop a zone to, say, 50% flow. That's well above the `≈0` no-flow
   floor, so today it passes clean while the bed slowly dries out. The band catches
   "this zone is running at 0.6× its normal" and flags it.
2. **Burst tape / blown fitting → over-watering + wasted rainwater.** A split drip line
   at 15 psi dumps water far above the zone's band. The "is the rate roughly nonzero?"
   check waves it through; the band flags "2× normal — burst?" — and on a finite
   rainwater tank, that's gallons you can't spare.
3. **Tanks empty vs. one clogged zone — discriminated.** When the rainwater tanks run
   dry, **every** zone no-flows in sequence; a clog is **one** zone. Today both surface
   as the same cryptic latched `E1`. Reading the *pattern across zones* lets the status
   surface say "all zones dry → likely SUPPLY EMPTY, refill the tanks," not "no-flow
   fault, is it a clog? a dead pump? a stuck valve?" — and it spares you cranking a
   self-priming pump against a dry line, run after run, all morning.

The bolder version (where this goes, not the first build): the per-zone EWMA *is* the
seasonal-fouling curve. Emitters lose a percent or two of flow a month to scale and
biofilm. Persist the band across a season and tinkle stops saying "zone 2 under-watered
today" and starts saying **"zone 2 has been drifting toward a clog for nine days — here's
the slope — flush it Saturday."** The single hall sensor, read as a *time series across
runs* instead of a scalar per run, is a whole plumbing diagnostician.

### Why it's worth it

- **Pure firmware leverage, available *now*.** Tinkle is parts-gated until Winter
  2026–27 — bench and wet validation can't start without hardware. This needs **zero new
  parts**: it's a core module on data the firmware already produces, fully host-testable
  with the existing fake-clock + injected-pulse harness. It's real work you can ship into
  the native-test tier months before a valve arrives.
- **It closes the actual field-failure gap.** Drip irrigation doesn't usually fail by
  flooding (the fail-dry architecture already owns that). It fails by *degrading* — a
  fouled emitter here, a rodent-chewed line there, a tank that ran low in a dry spell.
  Those are the failures that cost a crop, and they live in exactly the continuous middle
  of the flow signal that the binary checks ignore.
- **The pressure regulator makes it work.** Each tunnel has a fixed ≤15 psi regulator, so
  a zone's healthy steady-state GPM is genuinely stable run-to-run. That stability is what
  makes a learned per-zone band meaningful rather than noise — this is true *because* of
  tinkle's specific plumbing.

### Why he hasn't already

The honest reason — and it's the credible part. The entire flow subsystem was designed
through **one lens: fail-dry, prevent runaway-on.** Every flow decision answers "is too
much water moving, or is the pump dead?" Both built checks are flood/no-flood tripwires.
Even the hanging hook in firmware-spec §7 ("*optionally fault on rate far outside an
expected band*") is framed as a **fault** — a latch — and left unbuilt pending
calibration data.

Under-watering and slow degradation sit on a *different axis entirely*: agronomic health
and water budget, not flood safety. That axis was never in the frame that shaped the flow
code, so the rich middle of the signal was measured and discarded. The tell that this is
a real seam and not scope creep: DEC-014 already crossed exactly this line — it's a
flow-derived, non-latching *maintenance* signal (`ValveRestMonitor`). This proposal isn't
fighting the architecture; it's the second instance of a pattern Eric already blessed.

---

## The build handoff

### Approach

Build a **new `src/core` module, `FlowSignatureMonitor`**, as a **sibling to
`FlowFaultDetector`** — *not* folded into it. The split is load-bearing:

- `FlowFaultDetector` owns the **hard, latching** faults tied to RUNNING/IDLE
  (`raiseFault`). Untouched.
- `FlowSignatureMonitor` owns the **soft, self-healing health layer**
  (`FaultManager::note()` + `/api/status` flags). Never actuates — returns a verdict the
  caller routes, preserving `RunController`'s sole-commander invariant.

Mirror `ValveRestMonitor` beat-for-beat: ticked each loop with
`(runState, lastRunZone, rateGPM, pulses, now)`, returns a newly-flagged zone (or -1),
self-heals on a later clean run, surfaces via `flaggedMask()` / per-zone state to
`/api/status`. Keep **all** policy inside the module: the learning gate (what counts as a
clean run), the band math, and the classification thresholds. `RunController` decides
nothing about flow health.

**Learning gate (inside the module):** update a zone's EWMA band **only** from a run that
(a) reached `SETTLE` from `RUNNING` **without a fault**, (b) ran at least a minimum
duration (enough to reach steady state past `FLOW_GRACE_S`), and (c) was a **plain
(non-fert) run** — see the fertigation gotcha below. Until a zone has ≥ `MIN_CLEAN_RUNS`
samples it is "still learning" and is **not** classified (no false flags on a fresh
install or the unplumbed Zone 3).

**Classification (per completed run, on the SETTLE edge):** compare the run's observed
steady-state GPM (mean over the RUNNING window, which `FlowMonitor` already has the
samples for) against the zone's band:
- `< LOW_FRAC × band` (e.g. 0.6) and `> 0` → `FLOW_LOW` note (partial clog / fouling).
- `> HIGH_FRAC × band` (e.g. 1.5) → `FLOW_HIGH` note (burst / blown fitting).
- within band → clean; **clears** any standing flag for that zone (self-heal) and feeds
  the EWMA.

**Supply-empty discrimination (the cross-zone read):** keep a tiny per-zone "last run
no-flowed" bit. When a run ends at `≈0` GPM (the same condition `FlowFaultDetector` latched
on), record it. If **every plumbed zone** has no-flowed on its most recent run, annotate
the status surface / fault log with `SUPPLY_EMPTY` ("all zones dry → likely tanks, not a
clog"). **V1-safe framing:** this is an *annotation only* — it does **not** suppress the
existing `FAULT_NO_FLOW` latch or touch the pump gate. Fail-dry still stops the run; the
operator just reads the right story. (Pausing the schedule to spare the dry-running pump
is a real, attractive escalation — but it touches the safety-adjacent path and is a
*separate decision*, not this build.)

### File-by-file

- **`src/core/flow_signature_monitor.h` (new)** — the module. `Config` struct with the
  constants below (`#ifdef TINKLE_SIM` shortenings where a sim run is too short to reach
  steady state, same pattern as `FlowFaultDetector`/`ValveRestMonitor`). Public:
  `tick(RunState, uint8_t lastRunZone, float rateGPM, uint32_t pulses, uint32_t nowMs) -> int`
  (newly-flagged zone or -1); `zoneFlag(zone)` / `flaggedMask()`; `bandGPM(zone)` and a
  `learning(zone)` predicate for the status surface; `loadBands(...)` / a hook to seed the
  EWMA from NVS at boot. Private state: per-zone `bandGPM_[MAX_ZONES]`,
  `cleanRuns_[MAX_ZONES]`, `flagged_[MAX_ZONES]`, `lastRunNoFlow_[MAX_ZONES]`, and the
  RUNNING-window accumulation needed to compute a run's mean GPM at the SETTLE edge.
- **`src/core/flow_signature_monitor.cpp` (new)** — the state machine + EWMA + classifier.
  Watches for the `RUNNING → SETTLE` edge (like `ValveRestMonitor` watches close→rest),
  computes the run's steady-state mean, gates learning, classifies, updates flags. Pure;
  no GPIO, no interrupts.
- **`src/core/persistence.{h,cpp}`** — add per-zone band keys `z<N>_sig` (≤15 chars,
  float), write-on-change, read-with-default (an unset/zero band = "still learning", needs
  no migration — additive per DEC-008). A `swMaxRuntimeSec`-style getter/setter pair.
- **`src/core/fault_manager.h`** — add the note-only codes `FAULT_FLOW_LOW`,
  `FAULT_FLOW_HIGH`, `FAULT_SUPPLY_EMPTY` (E7/E8/E9 or next free), alongside the existing
  note-only `FAULT_VALVE_REST`. These go through `note()`, never `raiseFault` — they must
  not acquire a clear gate or a latch.
- **`src/esp32/main.cpp`** — instantiate `FlowSignatureMonitor`; seed bands from
  `Persistence` at boot; tick it each loop right beside `ValveRestMonitor` and
  `FlowFaultDetector`; route a newly-flagged zone to `FaultManager::note()` + a serial
  line; persist a zone's band when the EWMA updates (write-on-change).
- **`src/core/api.cpp` (`/api/status`)** — add a `flow.signature` block per zone:
  `{ band, lastRunGPM, flag (none|low|high), learning }` plus a top-level `supplyEmpty`
  bool. Pure additive JSON, host-tested against the real wire shape like the rest of `Api`.
- **`web/index.html` (SPA, display-only)** — render the per-zone signature + a
  "SUPPLY EMPTY — refill tanks" banner on the Status/Faults screens, beside the existing
  `valveRestFlags` service warnings. Thin client: it only *displays* the status fields;
  no logic. (Keep under the gzip budget — it's a few spans.)
- **`test/test_native/test_main.cpp`** — new cases (below).

### Constants (seeds — bench/season-tune, mark them as such per §15)

| Constant | Seed | Meaning |
|---|---|---|
| `EWMA_ALPHA` | 0.25 | band smoothing; lower = slower seasonal drift, more stable |
| `MIN_CLEAN_RUNS` | 3 | clean runs before a zone is classified |
| `LOW_FRAC` | 0.6 | below this × band → `FLOW_LOW` |
| `HIGH_FRAC` | 1.5 | above this × band → `FLOW_HIGH` |
| `MIN_LEARN_RUN_S` | 60 | a run must run at least this long to feed the band |

### Gotchas / risks

- **Fertigation shifts the signature.** A run through the Dosatron has a different
  pressure/flow profile than a plain run. **Don't** blend fert runs into the plain band.
  V1: learn and classify **plain runs only**; skip classification on the once-a-day fert
  run. (A separate per-zone fert band is a clean follow-on, not V1.)
- **Recalibration moves the GPM scale.** A new `K` from `/api/calibrate/finish` changes
  measured GPM. Decide explicitly: either reset the bands on calibration finish (simplest,
  honest — the EWMA re-converges over the next `MIN_CLEAN_RUNS`), or accept a few runs of
  drift. Recommend reset.
- **Zone 3 is wired-but-not-plumbed.** It will never reach a stable band until the hose
  line goes in. The "still learning" state must hold indefinitely without flagging — and
  the supply-empty cross-zone read must count **only plumbed** zones, or an unplumbed Z3
  permanently no-flowing would falsely satisfy "all zones dry." Gate the cross-zone read
  on a per-zone "plumbed/active" flag (config).
- **Cold start.** NVS persistence means a zone learns its band **once** and keeps it across
  reboots — good. But the very first season has a learning window where nothing is
  classified; that's correct, not a bug. Say so in the UI ("learning").
- **Keep the detector non-actuating.** The single biggest way to get this wrong is to let
  `FlowSignatureMonitor` reach for `raiseFault` or the pump. It must only ever `note()` and
  set status flags. The supply-empty annotation especially must not suppress the existing
  `FAULT_NO_FLOW` latch in V1.
- **Don't fold into `FlowFaultDetector`.** Tempting (shared inputs), wrong (mixes a
  latching safety path with a self-healing health path). Sibling modules.

### Done when

- `pio test -e native` green, including new cases: band convergence over clean runs;
  `FLOW_LOW` on an injected 0.5× run; `FLOW_HIGH` on a 2× run; no flag while "learning";
  self-heal (a clean run clears a standing flag and feeds the EWMA); learning gate (a
  faulted or sub-`MIN_LEARN_RUN_S` run does **not** corrupt the band); fert run skipped;
  cross-zone `SUPPLY_EMPTY` set only when every *plumbed* zone's last run no-flowed, and
  cleared by one good run.
- `pio run -e esp32` builds with the new module; the SPA still fits the <50 KB gzip gate.
- A Wokwi/`esp32_sim` session shows a flag raised from an injected low-flow run and cleared
  by a normal one, with `/api/status` reflecting it and the SPA rendering it.
- **No change** to `FlowFaultDetector`'s latching faults, `RunController`, the watchdog, or
  the pump-power gate. Diff against those files is empty.

### Kickoff (paste to a CC session on mill-dev)

> Implement `FlowSignatureMonitor` per `docs/proposals/flow-signature-health.md`. New
> `src/core` module, sibling to `FlowFaultDetector`, mirroring `ValveRestMonitor`'s
> non-latching DEC-014 shape: ticked each loop, returns a newly-flagged zone or -1, routes
> to `FaultManager::note()`, surfaces per-zone band + flags + a `supplyEmpty` bool to
> `/api/status`, learns a per-zone steady-state GPM band (EWMA, NVS-persisted `z<N>_sig`)
> from clean plain runs only. Classify low/high vs band, discriminate all-plumbed-zones-dry
> as SUPPLY_EMPTY (annotation only — do not touch the `FAULT_NO_FLOW` latch or the pump
> gate). Constants and the gotchas (fertigation, recalibration, unplumbed Z3,
> non-actuating) are in the doc. Host tests first. @architect first if the NVS-band or the
> cross-zone supply read needs a design call; then build it as one unit.
