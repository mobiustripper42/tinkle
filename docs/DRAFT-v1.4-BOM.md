# Tinkle V1 — Bill of Materials (DRAFT, post-v1.4 rearchitecture)

**For:** Red Tunnel build, on a chassis sized for three tunnels. Pairs with
`DRAFT-v1.4-valve-rearchitecture.md`. **Not yet propagated to the canonical docs.**
**Sourcing scope:** "now" = needed to build + bench + wet-confirm Red. "later" =
build-for-three headroom or plumbed-later. "reuse" = already on the shelf.

Headline change vs the old design: **5 motorized valves (4 NC + 1 NO), zero H-bridge
chips, no master valve.** Every valve is the same US Solid 2-wire auto-return family,
switched by a simple low-side driver.

---

## 1. Valves & water-side

| Item | Qty | Key spec | Candidate / link | Notes |
|---|---|---|---|---|
| Motorized ball valve, **NC** (normally-closed) | **3 now** (+1 later) | 3/4" brass, 9–36V AC/DC, 2-wire auto-return, ~2 W, 6–10 s, **NPT, standard port**, 2-indicator-light SKU | [US Solid brass NC](https://ussolid.com/products/3-4-motorized-ball-valve-brass-9-36v-ac-dc-2-wire-auto-return-electric-ball-valve-normally-closed-3-indicator-lights) | Now: Zone 1, Zone 2, Dosatron-leg. Later: Zone 3 (hose outlet). Draws holding current only while energized (open) during a run — negligible on the mains 24V supply. Lead-bearing brass → non-potable only (fine). |
| Motorized ball valve, **NO** (normally-open) | **1 now** | same family, **NPT, standard port**, 3-indicator-light SKU | [US Solid brass NO](https://ussolid.com/products/3-4-motorized-ball-valve-brass-electric-ball-valve-with-3-indicator-lights-2-wire-auto-return-normally-open-9-36v-ac-dc-by-u-s-solid-html) | Clean / plain-water bypass leg. Resting (de-energized) default = plain flows, Dosatron isolated. |
| Check valve, 3/4" | **reuse** | brass, 200 psi | existing **GASHER** | Dosatron **outlet — between the injector and the rejoin tee** (not after the tee — that would block the bypass). High-pressure side → cracking pressure irrelevant. Single check is proportional (rainwater, no city cross-connection — no RPZ/double-check). |
| Pressure regulator, ≤15 psi, 3/4" | 1 now | one per tunnel, upstream of zone split | Senninger PRL (or reuse if rated) | Drops to tape pressure before the zones. |
| Pump, SEAFLO 51 24V | 1 now | SFDP2-055-060-51, 5.5 GPM, switch set ~20 psi, internal bypass, run-dry | SEAFLO | Dead-51 on shelf = bench mule only. Buy a deployment unit. |
| Flex connectors (pump in + out) | 2 now | braided SS / reinforced, 12–18" | — | **Required** — never hard-plumb the glass-filled nylon ports. |
| Accumulator / expansion tank | reuse | pre-charged | shelf tank | Tames 5.5-vs-1.78 GPM short-cycle. |
| Filter, 100–140 mesh | reuse | 3/4" | existing | Before flow sensor + tape. |
| Dosatron D14MZ2 | reuse | 0.2–2%, 0.04–13.2 GPM | existing | |
| Hall flow sensor | 1 now | brass, 3/4", pulse out | TBD | After the merge, before the zone split. Calibrate K empirically. |
| Manual isolation valves | reuse/existing | both Dosatron legs | existing | Priming/service/winterize. |
| Unions / camlocks | several | pump, filter, both Dosatron sides | — | Serviceable string. |

---

## 2. Control board

| Item | Qty | Key spec | Candidate | Notes |
|---|---|---|---|---|
| ESP32 DevKitC, 38-pin | 1 | Arduino-ESP32 | — | Re-mapped pins (see draft §4). |
| ATtiny85 + programmer | 1 | watchdog MCU | Digispark / AVR-ISP | Dependency-free sketch. |
| Low-side valve driver | 1 per valve (5 now, build ~8–16) | discrete logic-level N-FET | **IRLZ44N** | Huge margin over cap inrush → inrush spec moot (don't bench it). Gate R + gate-to-GND pulldown so valves sit off at boot. |
| TVS clamp, drain–source per FET | 5 | e.g. **SMAJ30A** | SMAJ30A | Valves have an internal bridge rectifier → a freewheel diode across the valve won't clamp; clamp the **FET**, not the load. No RC snubber unless a scope shows ringing. |
| Flyback diode | pump-relay coil | 1N4007 / Schottky | — | Relay coil only — the valves use the per-FET TVS above, not a freewheel diode. |
| Pump relay (or MOSFET) | 1 | ~5–6 A, clean isolation | — | On the **armed** 24V (fail-dry source gate). |
| Safety/arm relay | 1 | NO, energize-to-pass, ≥10 A | — | ATtiny-armed; gates 24V to the **pump**. |
| TM1637 4-digit display | 1 | 2-wire | — | MM:SS countdown, read-only. |
| Flow-sensor level shift | 1 | 5V→3.3V (divider/module) | — | Protect GPIO27. |
| Enclosure | 1 | IP65+, fits 3-tunnel terminals **+ the 24V PSU inside**, clear/vented lid, DIN rail, glands | — | LRS PSU is IP20 → lives inside. |
| Terminal blocks + DIN rail | as needed | — | — | Build-for-three count. |

---

## 3. Power

| Item | Qty | Key spec | Candidate | Notes |
|---|---|---|---|---|
| 24V PSU | 1 | 24V, ≥6 A / 150 W, open-frame | Mean Well **LRS-150-24** | IP20 → inside the enclosure. LPV-150-24 if a sealed standalone is wanted. |
| Buck 24→5V | 1 | for flow sensor | — | |
| Buck 24→3.3V | 1 | for ESP32 + logic | — | (12V buck **dropped** — valves run on 24V.) |
| Inline fuse + holder | 1 | ~10 A on 24V out | — | |
| TVS across 24V | 1 | — | — | Brownout/transient insurance. |
| Reverse-polarity protection | 1 | diode or P-FET | — | |

---

## 4. Manual interface

| Item | Qty | Key spec | Candidate | Notes |
|---|---|---|---|---|
| Momentary button + **24V LED ring** | 3 | 19 mm, IP67, 24V (or 12–24V) ring | — | One per zone (Z1/Z2 + Z3 hose), DEC-006. Ring runs straight off the 24V bus through its FET. |
| LED-ring low-side switch | 3 | small N-FET / logic-level transistor per ring | — | GPIO can't sink a 24V ring directly; one switch per ring off 24V. (No ULN2803, no 12V rail.) |

---

## 5. Wiring & misc

| Item | Qty | Key spec | Notes |
|---|---|---|---|
| Direct-burial / UF irrigation cable | as needed + spares | 18 AWG, conductor count per final pin map | Each valve = 2 conductors (on/off) + common. |
| Waterproof splice connectors | as needed | gel- or silicone-filled | No bare wire nuts. |

---

## Open items

**Resolved (sourcing pass):**
1. ✅ Driver = **IRLZ44N FET per valve** — margin makes the cap-inrush question moot.
2. ✅ Clamp = **SMAJ30A TVS drain–source per FET** (internal-rectifier valves; no RC).
3. ✅ LED rings = **24V**, one low-side switch per ring off the 24V bus (no 12V, no ULN2803).
4. ✅ Check valve = **existing GASHER**, Dosatron outlet between injector and rejoin tee
   (200 psi, single check sufficient — rainwater, no cross-connection).

**Confirm at order:**
- Valve SKUs: NO clean leg = **3-indicator-light**, NC fert + zone legs = **2-indicator-light**;
  **NPT (not BSP)**, standard port on all.

**✅ Master valve — confirmed OUT** (architect review): pump-off is the complete flood gate;
the master's only unique job (siphon block) doesn't apply here (negligible head +
reverse-checking pump). No master in the BOM.

## Net delta vs the pre-v1.4 design

**+** 2 motorized valves (5 vs 3), 1 check valve, ~5 low-side driver channels.
**−** 4 DRV8871 H-bridges, 1 master solenoid (WIC 2BCW) + its FET, the 12V buck.
