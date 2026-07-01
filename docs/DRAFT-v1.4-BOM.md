# Tinkle V1 — Bill of Materials (DRAFT, post-v1.4 rearchitecture)

> **⚠ Superseded in part by DEC-019 (v1.5, phone-only).** Any line items for the **TM1637 display, the 3 momentary buttons, the 3 LED rings**, and their 24 V ring rail / driver are **cut** — V1 has no on-box panel, only a single onboard alive LED (GPIO2). An **AC master switch** on the Mean Well input is the optional service disconnect. Treat the panel rows below as historical.
>
> **⚠ Also reconciled by DEC-020 (v1.5 hardware).** Rows updated inline (look for the *v1.5 (DEC-020)* tag): pump **51→55**, flow sensor **→ Leridian**, TVS **SMAJ30A→1.5KE30A through-hole**, **24→3.3V buck removed** (single 5V buck; ESP32 self-makes 3.3V), **PSU mounts outside**, enclosure **→ opaque Boxco P-series**, hybrid protoboard build.

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
| Pump, SEAFLO **55** 24V | 1 now | SFDP-055-060-55, 7.0 GPM, switch set ~20 psi, internal bypass, run-dry, heavy-duty pressure switch + anti-vibration mounts | SEAFLO | **v1.5 (DEC-020) — was 51.** Dead-51 on shelf = bench mule only. Buy a deployment 55. |
| Flex connectors (pump in + out) | 2 now | braided SS / reinforced, 12–18" | — | **Required** — never hard-plumb the glass-filled nylon ports. |
| Accumulator / expansion tank | reuse | pre-charged | shelf tank | Tames 7.0-vs-1.78 GPM short-cycle (worse with the 55). |
| Filter, 100–140 mesh | reuse | 3/4" | existing | Before flow sensor + tape. |
| Dosatron D14MZ2 | reuse | 0.2–2%, 0.04–13.2 GPM | existing | |
| Hall flow sensor | 1 now | **Leridian 3/4" NPT**, 2–45 L/min, 253 psi, pulse out | Leridian | **v1.5 (DEC-020).** After the filter, after the merge, before the zone split. Won't register <2 L/min (zone ≈6.7 L/min). Seed K = Leridian; calibrate empirically. |
| Manual isolation valves | reuse/existing | both Dosatron legs | existing | Priming/service/winterize. |
| Unions / camlocks | several | pump, filter, both Dosatron sides | — | Serviceable string. |

---

## 2. Control board

| Item | Qty | Key spec | Candidate | Notes |
|---|---|---|---|---|
| ESP32 DevKitC, 38-pin | 1 | Arduino-ESP32 | — | Re-mapped pins (see draft §4). |
| ATtiny85 + programmer | 1 | watchdog MCU | Digispark / AVR-ISP | Dependency-free sketch. Runs at **3.3V**. Flash via Arduino-as-ISP (buildup Step 8.0). |
| Watchdog support passives | per ATtiny | 2× 10k | — | **10k pin7→GND** (heartbeat pull-down — safety-critical: a broken heartbeat must read as quiet so the pump disarms); **10k RESET→3.3V** (glitch immunity). The trip-line **10k pin36→3.3V + 100nF pin36→GND** are the *same parts already listed for Step 1* — not additional. |
| Low-side valve driver | 1 per valve (5 now, build ~8–16) | discrete logic-level N-FET | **IRLZ44N** | Huge margin over cap inrush → inrush spec moot (don't bench it). Gate R + gate-to-GND pulldown so valves sit off at boot. |
| TVS clamp, drain–source per FET | 5 | **1.5KE30A (through-hole)** | 1.5KE30A | **v1.5 (DEC-020) — was SMAJ30A SMD.** Valves have an internal bridge rectifier → a freewheel diode across the valve won't clamp; clamp the **FET**, not the load. No RC snubber unless a scope shows ringing. |
| Relay module, 1-channel opto | 2 (sold as a 4-pack; 2 spare) | 5V VCC, opto-isolated, **H/L jumper set HIGH**, triggers at 3.3V, relay = SRD-05VDC-SL-C, 10A contacts | Amazon — "1 channel 5V relay module optocoupler high/low trigger" | **v1.5 — the pump-switching relays (were unspecified placeholders).** Two **independent** boards (pump + safety) — better than a shared 2-channel board for the two-key chain. Each: `VCC`→5V, `GND`→common, `IN`←control pin, contacts `COM`/`NO` in series on the pump feed. Safety `IN`←ATtiny pin 6 (**+ 10k IN→GND**, holds it off during ATtiny reset), pump `IN`←ESP32 pin 22. **Set the jumper HIGH** (boot-LOW = off = fail-dry) and **bench-confirm the 3.3V trigger** before wiring the pump. Module has its own coil driver + flyback — no discretes. |
| TM1637 4-digit display | 1 | 2-wire | — | MM:SS countdown, read-only. |
| Flow-sensor level shift | 1 | 5V→3.3V (divider/module) | — | Protect GPIO27. |
| Enclosure | 1 | **opaque grey Boxco P-series (~170×220×100)**, IP65+, UV-stable, vertical mount, glands down, DIN rail | Boxco P-series | **v1.5 (DEC-020) — was clear-lid w/ PSU inside.** Houses controller/driver/terminals only; the PSU mounts **outside**. |
| Terminal blocks + DIN rail | as needed | **DIN lever blocks** + jumper bars (24V+/GND rails) | on hand | Build-for-three count. Hybrid build: ESP32 on screw-terminal breakout; bare FETs + socketed ATtiny on ElectroCookie protoboard. |

---

## 3. Power

| Item | Qty | Key spec | Candidate | Notes |
|---|---|---|---|---|
| 24V PSU | 1 | 24V, ≥6 A / 150 W, open-frame | Mean Well **LRS-150-24** | **v1.5 (DEC-020):** IP20 → mounts **outside** the enclosure on its own bracket (shaded/vented). LPV-150-24 if a sealed standalone is wanted. |
| Buck 24→5V | 1 | feeds flow sensor **and ESP32** (5V/VIN → onboard 3.3V; 3V3 pin = logic rail) | — | **v1.5 (DEC-020):** the only buck. 24→3.3V buck **removed**; 12V buck already dropped — valves run on 24V. |
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
2. ✅ Clamp = **1.5KE30A (through-hole) TVS drain–source per FET** (internal-rectifier valves; no RC). *(v1.5 / DEC-020 — was SMAJ30A SMD.)*
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
