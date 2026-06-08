# Tinkle — v1.4 Valve Re-Architecture (DRAFT for review)

**Status:** proposal — not yet propagated to the canonical docs (SPEC, DECISIONS,
hardware/firmware specs, wiring). Drafted for `@architect` review and to seed a BOM.
Supersedes the v1.3 zone-valve scheme (and the DEC-011 currently committed, which
describes reverse-polarity hold-position valves — wrong part).

**One-line:** every actuated valve becomes a **2-wire on/off motorized ball valve**
(US Solid family), the 3-way diverter splits into **two 2-way valves**, the **master
valve is removed**, and the board loses every H-bridge. Fail-dry moves from "the NC
master springs closed" to "**the pump has no power**" (source control).

---

## 1. What changed from v1.3, and why

| | v1.3 (committed, wrong part) | v1.4 (this draft) |
|---|---|---|
| Zone valves | reverse-polarity, **hold position** on power loss | **NC auto-return** — closed when unpowered, opens energized, cap-returns closed |
| Zone driver | DRV8871 **H-bridge**, 2 GPIO/valve | **low-side FET**, 1 GPIO/valve (like the master) |
| Diverter | one motorized **3-way** (reverse-polarity) | **two 2-way** valves: NO clean leg + NC Dosatron leg, + check valve |
| Master valve | WIC 2BCW NC solenoid | **removed** |
| H-bridges on board | 3 zones + 1 diverter = 4 | **zero** |
| Actuator rail | 24V raw + 12V pulse buck | **single armed 24V**; 12V buck dropped |
| Fert state | cached diverter position (DEC-008), travel-on-change | per-run: energize the right leg; no cache |

**Driver of the change:** the actual part Eric is sourcing is the US Solid 3/4" brass
**2-wire auto-return, normally-closed** motorized ball valve (9–36V AC/DC, ~2 W,
6–10 s travel, **capacitor** return). It is on/off, not reverse-polarity — so the
H-bridge model and the v3 "one H-bridge family" framing don't apply. Re-thinking from
there collapsed the design to something simpler.

---

## 2. Water path (Red, V1)

```
Rainwater tanks (west)
   → SEAFLO 51 pump (self-priming, pressure switch, internal bypass, no reverse flow)
   → [ union / camlock ]
   → accumulator (shelf expansion tank)
   → filter (100–140 mesh) → [ union / camlock ]
   → TEE ─┬─ clean leg:  NO ball valve ───────────────────────────────┐
          └─ fert  leg:  NC ball valve → Dosatron D14MZ2 → check valve ─┤
                                                                  → MERGE
   → flow sensor (hall-effect)
   → pressure regulator (one per tunnel, ≤15 psi)
   → zone split → [ Zone 1 NC valve → beds 1–3 ]
                  [ Zone 2 NC valve → beds 4–6 ]
                  [ Zone 3 NC valve → hose outlet (wired now, plumbed later) ]
   → AquaTraxx drip tape
```

- **No master.** The pump is the source; gating its power is the gate (§3).
- **Diverter = two 2-ways.** Clean/plain leg = **normally-open**; Dosatron/fert leg =
  **normally-closed**. Unpowered rest state → clean open, fert closed: exactly one path
  open (no deadhead), fails safe to plain water. A **plain run powers nothing**; a fert
  run energizes the NC fert leg open and the NO clean leg closed.
- **Check valve** on the Dosatron outlet (passes toward the merge, blocks reverse) so
  bypass flow can't back-feed/back-pressurize the injector during plain runs. Inlet NC
  valve + outlet check = fully isolated fert leg when off.
- **Flow sensor + regulator** sit on the common merged line before the zone split — one
  sensor reads whichever zone runs; one regulator per tunnel serves all zones.

---

## 3. Fail-dry architecture (restated without the master)

**The backbone is source control, not a valve.** The ATtiny-armed safety relay (NO,
energize-to-pass) gates 24V to the **pump** (the source), and is armed only while
a run is active (DEC-003 heartbeat encoding). No run → no pump power → no pressure →
no water, regardless of any valve's state. The pump's internal check valves block
reverse/siphon flow when it is off (confirmed: water cannot flow back through the pump;
tanks give only a trickle of head when full — siphon is a non-issue).

**Two-key chain (unchanged in spirit):**
- **ESP32** commands each actuator (energizes the chosen zone + diverter legs, enables
  the pump).
- **ATtiny85** arms the safety relay feeding the **pump** (the source), armed only during
  runs. Lose either — power, heartbeat, the relay — and the pump de-powers, so no water
  moves. **The pump on the armed rail is the gate**; valve state is irrelevant to dryness (a
  valve can't pass water with no pump pressure). The valves rest closed when de-energized as
  a convenience, not as a safety barrier.

**The valves are not a safety barrier.** NC zones rest closed and the NO bypass rests open
when de-energized — a sensible resting state and the correct fert default, not a fail-dry
layer. A valve can't pass water with the pump unpowered, so for the runaway/flood guarantee
the *source gate is the whole story*; valve state only decides *which* beds get water during
a bounded run.

**Removing the master loses no real protection.** Pump-off is the complete flood gate: no
pump → no pressure → no water, whatever the valves do. The master's one unique job —
blocking a gravity siphon through a stuck-open valve while the pump is off — doesn't apply
here: near-zero tank head + the SEAFLO's internal checks block reverse flow. So it's out,
cleanly. What *does* stay load-bearing is **DEC-003's "heartbeat = run-active" idle-disarm**
— it's why the pump is unpowered between runs; an always-on heartbeat would break that, so
that encoding is now a safety requirement.

**Auto-return self-test (a requirement — for correctness, not safety):** the firmware
periodically verifies each valve rests closed, so a stuck-open or degraded valve gets
*noticed* (don't silently water the wrong bed; flag the valve for service). It is **not** a
fail-dry barrier — the source gate is.

### Failure-mode table (revised)

| Failure | Result | Why it's safe |
|---|---|---|
| Mains loss | Pump dead → no source (valves also rest closed, but the dead pump is what makes it dry) | No water. Source gone. |
| Firmware hang / watchdog trip | Safety relay opens → **pump** de-powered → no source → no water (valve state irrelevant to dryness) | No runaway-on. |
| Single zone valve stuck open | Between runs pump is unpowered → no source → no flow; flow sensor flags idle flow | Source-gated; bounded to commanded runs. |
| Pump-enable relay welded on | Pump only powered while the safety relay is armed (during runs); idle = no power | Safety relay sits in series ahead of the enable — it still de-powers the pump at idle. |
| Safety relay welded armed **and** enable welded **and** a zone stuck open | Possible idle flow | 3 independent faults to flood; flow sensor flags idle flow. |
| Zone capacitor degraded (won't auto-close) | Zone may stay open after a run; pump off between runs → no flow | Source-gated; bounded; flow-flagged. Periodic auto-return test. |
| First ~1 min after boot (caps not charged) | A zone may not auto-close on an immediate power loss | Same event de-powers the pump → no source. |
| Diverter wrong leg / both open | Wrong fert state | Agronomic oops, not a flood; source-gated. |

Net: flooding requires the pump powered when it shouldn't be **and** an open path. The
source's hardware gate is the armed safety relay (pump-enable in series behind it — a welded
*enable* is still caught; a welded *safety relay* is the single hardware point that defeats
it, with the software flow check behind that). Runs are time-bounded by the watchdog
ceiling, so even a wrong or extra open valve during a run waters a bounded amount, never a
runaway. The master is out with **no loss** — its only unique job, blocking a siphon through
a stuck-open valve, doesn't apply here (negligible head + reverse-checking pump).

---

## 4. Control hardware

**Valve driver — one low-side N-FET per valve** (IRLZ44N; huge margin over the cap inrush,
so the inrush spec is moot), gate resistor + gate-to-GND pulldown so each valve sits **off**
through ESP32 boot. **Clamp the FET, not the load:** a TVS (SMAJ30A) drain-to-source per
FET — the valves have an internal bridge rectifier, so a freewheel diode across the valve
won't clamp cleanly. The pump-relay coil keeps its flyback diode. No H-bridges, no DRV8871,
no never-both-high invariant. Build ~8–16 channels (build-for-three); **populate 5** for
Red V1.

**Valve channels populated (Red V1):** Z1, Z2, Z3 (NC) + clean bypass (NO) + Dosatron
leg (NC) = **5**.

**Proposed pin map (re-map — replaces the v1.3/DEC-007 H-bridge layout):**

| GPIO | Dir | Connects to | Note |
|---|---|---|---|
| 13 | OUT | Zone 1 valve FET | gate pulldown, boot-low = off |
| 14 | OUT | Zone 2 valve FET | |
| 16 | OUT | Zone 3 valve FET (hose outlet) | |
| 17 | OUT | Clean-bypass valve FET (NO) | |
| 18 | OUT | Dosatron-leg valve FET (NC) | |
| 22 | OUT | Pump relay trigger | |
| 27 | IN  | Flow sensor pulse | interrupt; level-shift 5V→3.3V |
| 25 / 26 | OUT | TM1637 CLK / DIO | |
| 34 / 35 / 39 | IN | Buttons B1 / B2 / B3 | input-only, ext pull-up + debounce cap |
| 32 / 33 / 23 | OUT | LED rings 1 / 2 / 3 → per-ring low-side switch | 24V rings off the 24V bus |
| 4 | OUT | Heartbeat → ATtiny | |
| 36 | IN | ATtiny "tripped" → ESP32 | input-only |

Freed vs v1.3: GPIO21 (old master FET), GPIO12/15 (old Z3 H-bridge strapping pins),
GPIO19 (old diverter IN2). **DEC-007's strapping-pin contortion is retired** — we no
longer need GPIO12. Strapping pins are now only used if a gate-pulldown'd FET lands on
one (boot-safe by the same master-FET pattern), but the proposed map avoids them.

**Power rails:** one 24V supply, two sides of the safety relay. **Pump on the armed
(gated) 24V** — the fail-dry source gate. **Valves on raw 24V** (as today's zones already
are): minimal wiring change, and it sidesteps a de-arm-vs-`CLOSE_ZONE` timing race; the
source gate (pump) is what makes it dry, so the valves don't need gating. Valves are
9–36V, ~2 W → 24V direct, **no 12V buck** (button LED rings are now 24V rings, one low-side
switch each off the 24V bus). Keep **24→5V** (flow sensor) and **24→3.3V** (logic). Per-FET
TVS clamp (SMAJ30A) on each valve channel, flyback on the pump relay, inline fuse, TVS,
reverse-polarity protection.

**Watchdog:** unchanged in principle (ATtiny + safety relay, DEC-003), except the relay
now gates the **pump** alone (the source) instead of "master + pump" — master gone, valves
on raw 24V. **DEC-003's idle-disarm is now load-bearing** (see §3). A trip de-powers the
**pump** (the source) → no water; valve state is irrelevant to dryness.

---

## 5. Firmware impact (grows the rework task — re-estimate)

- **State machine:** drop `OPEN_MASTER` / `CLOSE_MASTER`. New sequence:
  `PREP_DIVERTER → OPEN_ZONE → START_PUMP → RUNNING → STOP_PUMP → CLOSE_ZONE → SETTLE`.
  `PREP_DIVERTER` sets the two diverter legs per the fert flag (plain = de-energize both;
  fert = energize fert-open + bypass-closed), waits travel.
- **`ValveDriver`:** becomes N on/off channels each with a travel-complete timer. No
  H-bridge, no `pulseOpen/pulseClose`, **no never-both-high invariant** (no input pairs).
  `openZone/closeZone` = set/clear a FET + start a `ZONE_TRAVEL_MS` timer; `zoneBusy()`
  still gates the sequence (already correct).
- **Fert logic:** no cached diverter position (DEC-008 simplifies). Keep a
  "don't re-toggle if already in the wanted state" guard to avoid needless travel/wear.
- **Diverter leg-switch transient:** the two legs take ~5–15 s to travel, so a fert↔plain
  switch briefly overlaps (both-open or both-closed for a few seconds). Harmless — the pump
  rides a short deadhead on its internal bypass, and a brief split just under-injects for a
  moment. Firmware *may* stagger the two (make-before-break) for cleanliness; not required.
- **Constants:** `ZONE_TRAVEL_MS ≈ 10000` (6–10 s spec + margin; bench-confirm), same for
  the diverter legs. `PULSE_MS` and the H-bridge config are gone.
- **Boot/cap note:** a valve may not fully auto-close in the first ~1 min after boot (cap
  not yet charged). Not a safety issue — the source gate covers power loss; the self-test
  flags a valve that didn't close. Document it.

This is no longer a rename — it's a re-architecture of `ValveDriver` + `RunController` +
`pins.h` + the native tests. The plan's task 1.7 should be re-scoped and re-estimated
(likely 5–8, not 3).

---

## 6. Decisions affected (for the eventual doc pass — "docs last")

- **DEC-011** — rewrite from scratch: NC auto-return zones, no master, two-valve diverter,
  source-control fail-dry.
- **DEC-007** — **retire** (no H-bridge, no strapping-pin pressure).
- **DEC-008** — drop the cached-diverter-position machinery; fert state is set per-run.
- **DEC-003** — amend, and the amendment is **load-bearing**: relay gates the pump (source);
  the no-master argument *depends on* heartbeat-means-run-active (idle disarm is now a safety
  requirement, not just a runtime-ceiling convenience).
- **New DEC** — commit the **auto-return self-test** as a requirement, framed as
  *correctness* (catch a stuck-open / degraded valve), **not** a fail-dry barrier. Record the
  capacitor caveats (1-min charge after open, aging).
- **Hardware spec §4** — the line *"single 3-way preferred over two 2-way solenoids (avoids
  the both-closed deadhead)"* is **superseded**: that reasoned about NC+NC; **NO-clean +
  NC-fert** defaults to exactly one path open, so there is no deadhead.
- **Firmware spec §17** — drop *"never both H-bridge inputs high"*; add: (a) every NC valve
  closes within X s of de-energize, (b) the auto-return self-test flags a degraded cap, (c) a
  power loss in the first 60 s still leaves the system dry via the source gate.

---

## 7. Open items before freeze

**Resolved at sourcing:** valve low-side driver = IRLZ44N FET per valve (cap-inrush moot);
clamp = SMAJ30A TVS drain–source per FET (internal-rectifier valves; no RC); LED rings = 24V,
one low-side switch per ring off the 24V bus; check valve = existing GASHER.

- Confirm valve SKUs at order: NO clean leg = 3-indicator-light, NC fert + zone legs =
  2-indicator-light; **NPT (not BSP)**, standard port on all.
- Bench-confirm travel time and cap behavior (Phase 6).
- Lead-bearing brass: "not for drinking water" — fine for non-potable rainwater; noted.

---

## 8. Preliminary component spine (to grow into the BOM)

| Item | Qty (Red V1) | Note |
|---|---|---|
| US Solid 3/4" brass NC 2-wire auto-return ball valve | 4 | Z1, Z2, Z3, Dosatron-leg |
| US Solid 3/4" brass **NO** 2-wire auto-return ball valve | 1 | clean bypass |
| 3/4" check valve | 1 | Dosatron outlet |
| Low-side logic-level N-FET (e.g. IRLZ44N) + gate R + pulldown | 5 populated (build more) | one per valve |
| Flyback diode (1N4007 / Schottky) | per valve + pump relay | |
| Pump relay (or MOSFET, ~5A) | 1 | |
| Safety relay (NO, energize-to-pass) | 1 | gates 24V to the pump (the source) |
| ESP32 DevKitC 38-pin | 1 | |
| ATtiny85 (watchdog) | 1 | |
| SEAFLO 51 pump (24V) | 1 | |
| Accumulator / expansion tank | 1 | reuse shelf tank |
| Filter 100–140 mesh | 1 | reuse |
| Pressure regulator (≤15 psi, 3/4") | 1 | one per tunnel |
| Dosatron D14MZ2 | 1 | existing |
| Hall flow sensor (brass 3/4") | 1 | |
| TM1637 4-digit display | 1 | |
| IP67 momentary button + LED ring | 3 | |
| Mean Well LRS-150-24 (24V PSU) | 1 | |
| Buck: 24→5V, 24→3.3V | 1 each | 12V buck dropped |
| Manual isolation valves, unions/camlocks, fuse, TVS, burial wire | as needed | |

> **BOM note:** the valve count is the headline change — **5 motorized valves** (4 NC +
> 1 NO) instead of 3 zone valves + a 3-way diverter, and **zero H-bridge driver chips**
> (5 FETs instead). Net parts: +2 valves, −4 DRV8871s, −1 master solenoid, −1 master FET,
> −1 12V buck, +1 check valve.

---

## 9. Additional V1 requirement (this session) — flow-check manual override

Not a valve item, but captured here so it isn't lost; lands in the **firmware spec** at the
doc pass (§7 FlowMonitor, §10 `/api/settings` + §10.1 settings screen + status banner,
§14 fault gating) and in the Phase 3/4 plan tasks.

**Requirement:** a web-UI setting that disables the FlowMonitor **faults** (`NO_FLOW` and
unexpected-idle-flow) so a missing, uncalibrated, or misbehaving flow sensor can't block
watering. Fail-dry cuts both ways — a bad sensor that halts all irrigation is its own
failure mode.

**Behavior / safety boundaries:**
- **Mutes faults, not measurement:** flow is still counted (gallons logged) and shown live;
  only the fault/stop action is suppressed.
- **Software-only — cannot touch the hardware fail-dry:** no effect on the watchdog, safety
  relay, pump-power gate, or `HARD_MAX_RUNTIME`. With flow checks off, a run still can't
  exceed the runtime ceiling and still fails dry on power loss / hang. (Consistent with
  §10.1: the SPA has no role in fail-dry.) Worst case = a run finishes its commanded
  duration without the cross-check — bounded, never a runaway.
- **Default ON (override off); persisted in NVS** (needed for the "sensor not installed
  yet" case). Enabling it clears any latched flow fault and prevents re-fault while active.
- **Loud + visible:** persistent "⚠ FLOW CHECK DISABLED" banner on every screen + a status
  API flag + a fault-log entry on toggle. A silently-muted safety check is the hazard.

**Open choice:** single toggle (mute both faults) vs. granular (mute `NO_FLOW`, keep the
unexpected-idle-flow leak detector always on). Recommend single toggle for V1; granular as a
refinement. *Auto-revert after a window* is a possible future safeguard against "left it off
forever," not required for V1.
