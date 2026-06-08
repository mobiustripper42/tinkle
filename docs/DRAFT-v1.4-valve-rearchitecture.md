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
energize-to-pass) gates 24V to the pump **and** the valve rail, and is armed only while
a run is active (DEC-003 heartbeat encoding). No run → no pump power → no pressure →
no water, regardless of any valve's state. The pump's internal check valves block
reverse/siphon flow when it is off (confirmed: water cannot flow back through the pump;
tanks give only a trickle of head when full — siphon is a non-issue).

**Two-key chain (unchanged in spirit):**
- **ESP32** commands each actuator (energizes the chosen zone + diverter legs, enables
  the pump).
- **ATtiny85** arms the safety relay feeding the **pump** (the source), armed only during
  runs. Lose either — power, heartbeat, the relay — and the pump de-powers, so no water
  moves. NC valves close on their own de-energize and on total power loss; the pump on the
  armed rail is the gate, the valves are a secondary layer.

**Secondary layers (defense-in-depth, not load-bearing):**
- NC zone valves cap-return closed; the NO bypass returns to plain.
- These are *soft*: the cap needs ~1 min of power to arm after the valve opens, and ages
  over years in heat. Acceptable precisely because the *source* (pump power), not the
  valve, is the trusted barrier.

**Honest trade (per @architect review — not a free fail-dry wash):** removing the master
swaps one *independent mechanical* barrier (the NC master, downstream of everything) for
the source-gate backbone — **one** hardware gate, the armed relay, with the pump-enable in
*series* behind it (so a welded *enable* is caught, but they are **not** two independent
gates) — plus the soft, aging, boot-windowed NC-cap zone layer and the software flow-sensor
cross-check. A welded **safety relay** is then the single point that defeats source gating,
caught only by the flow sensor. Defensible for a non-potable rainwater system with
negligible head and a reverse-checking pump — but it **requires** the self-test below, and
it makes DEC-003's "heartbeat = run-active" idle-disarm **load-bearing** (an always-on
heartbeat would remove the idle no-source guarantee the master used to provide).

**Auto-return self-test (a requirement, not a nicety):** since the NC cap-return is now the
only mechanical fail-closed element, the firmware must periodically exercise and verify each
valve's close (commanded close watched on the flow sensor / position indicator) so a
degraded cap is caught *before* it matters.

### Failure-mode table (revised)

| Failure | Result | Why it's safe |
|---|---|---|
| Mains loss | Pump dead → no source; NC valves cap-close; NO bypass returns open | No water. Source gone. |
| Firmware hang / watchdog trip | Safety relay opens → pump + valve rail de-powered → pump off, valves close | No runaway-on. |
| Single zone valve stuck open | Between runs pump is unpowered → no source → no flow; flow sensor flags idle flow | Source-gated; bounded to commanded runs. |
| Pump-enable relay welded on | Pump only powered while the safety relay is armed (during runs); idle = no power | Safety relay is an independent second gate on the source. |
| Safety relay welded armed **and** enable welded **and** a zone stuck open | Possible idle flow | 3 independent faults to flood; flow sensor flags idle flow. |
| Zone capacitor degraded (won't auto-close) | Zone may stay open after a run; pump off between runs → no flow | Source-gated; bounded; flow-flagged. Periodic auto-return test. |
| First ~1 min after boot (caps not charged) | A zone may not auto-close on an immediate power loss | Same event de-powers the pump → no source. |
| Diverter wrong leg / both open | Wrong fert state | Agronomic oops, not a flood; source-gated. |

Net: flooding requires the pump powered when it shouldn't be **and** an open path. The
source's hardware gate is the armed safety relay, with the pump-enable in *series* behind
it (a welded *enable* is still caught, but they are **not** independent — a welded *safety
relay* defeats both, leaving only the software flow check). The master's one unique
remaining job — blocking a gravity siphon through a stuck-open zone — is moot here (pump
checks reverse flow, negligible head), which is why it can go; but see the honest-trade
note above for what the removal actually costs.

---

## 4. Control hardware

**Valve driver — one low-side N-FET per valve** (logic-level, e.g. IRLZ44N), gate
resistor + gate-to-GND pulldown so each valve sits **off** through ESP32 boot. Flyback
diode across each valve (motor + cap load) and the pump relay coil. No H-bridges, no
DRV8871, no never-both-high invariant. Build ~8–16 channels (build-for-three); **populate
5** for Red V1.

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
| 32 / 33 / 23 | OUT | LED rings 1 / 2 / 3 | drive sorted at button BOM (24V via resistor or logic rail) |
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
9–36V, ~2 W → 24V direct, **no 12V buck**. Keep **24→5V** (flow sensor) and **24→3.3V**
(logic). Per-valve snubber/clamp for the motor+cap load (open item — real answer before
BOM freeze), flyback on the pump relay, inline fuse, TVS, reverse-polarity protection.

**Watchdog:** unchanged in principle (ATtiny + safety relay, DEC-003), except the relay
now gates the **pump** alone (the source) instead of "master + pump" — master gone, valves
on raw 24V. **DEC-003's idle-disarm is now load-bearing** (see §3). A trip de-powers the
source and lets the NC valves close.

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
- **Constants:** `ZONE_TRAVEL_MS ≈ 10000` (6–10 s spec + margin; bench-confirm), same for
  the diverter legs. `PULSE_MS` and the H-bridge config are gone.
- **Boot caveat:** document the ~1-min cap-charge window — valves can't be trusted to
  auto-close on power loss until charged; the source-gate covers it, but it's written down.

This is no longer a rename — it's a re-architecture of `ValveDriver` + `RunController` +
`pins.h` + the native tests. The plan's task 1.7 should be re-scoped and re-estimated
(likely 5–8, not 3).

---

## 6. Decisions affected (for the eventual doc pass — "docs last")

- **DEC-011** — rewrite from scratch: NC auto-return zones, no master, two-valve diverter,
  source-control fail-dry.
- **DEC-003** — safety relay gates **pump + valve rail** (was master + pump); fail-dry
  language shifts from "NC master springs closed" to "pump de-powered + NC valves close."
- **DEC-007** — **retire** (no H-bridge, no strapping-pin pressure).
- **DEC-008** — drop the cached-diverter-position machinery; fert state is set per-run.
- **DEC-003** — amend, and the amendment is **load-bearing**: relay gates the pump (source);
  the no-master argument *depends on* heartbeat-means-run-active (idle disarm is now a safety
  requirement, not just a runtime-ceiling convenience).
- **New DEC** — commit the **auto-return self-test** as a requirement (now the only
  pre-failure detector for the sole mechanical fail-closed element). Record the capacitor
  caveats (1-min charge after open, aging).
- **Hardware spec §4** — the line *"single 3-way preferred over two 2-way solenoids (avoids
  the both-closed deadhead)"* is **superseded**: that reasoned about NC+NC; **NO-clean +
  NC-fert** defaults to exactly one path open, so there is no deadhead.
- **Firmware spec §17** — drop *"never both H-bridge inputs high"*; add: (a) every NC valve
  closes within X s of de-energize, (b) the auto-return self-test flags a degraded cap, (c) a
  power loss in the first 60 s still leaves the system dry via the source gate.

---

## 7. Open items before BOM / freeze

- LED-ring drive voltage (24V via series resistor vs a logic rail).
- FET + gate-network values; confirm flyback/snubber for the motor+cap valve load.
- Confirm US Solid sells the **normally-open** brass variant in the same size (the clean
  bypass) — search says yes; confirm SKU at sourcing.
- Bench-confirm travel time and cap behavior (Phase 6).
- Lead-in brass: "contains lead, not for drinking water" — fine for non-potable rainwater
  irrigation; noted.

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
| Safety relay (NO, energize-to-pass) | 1 | gates 24V to pump + valve rail |
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
