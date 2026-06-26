# Tinkle — Irrigation Controller
## Hardware & System Specification, V1

**Status:** Sourcing — water-side parts selected; controller BOM in progress
**Build target:** Winter 2026–27, running the 2027 season
**Scope of V1:** Red Tunnel only, built on a chassis sized for three tunnels

**Changelog (v1.5):**
- **Hardware reconcile (DEC-020).** Six as-built corrections from the design chat: **(1)** the **24→3.3V buck is removed** — a single 24→5V buck feeds the flow sensor *and* the ESP32 (fed 5V on 5V/VIN, it makes its own 3.3V; the 3V3 pin sources logic). **(2)** Pump is now the **SEAFLO 55** (SFDP-055-060-55, 7.0 GPM, heavy-duty pressure switch + anti-vibration mounts), superseding the 51. **(3)** Flow sensor is the **Leridian 3/4" NPT** hall unit (2–45 L/min; won't register below 2 L/min). **(4)** Per-FET TVS is the **through-hole 1.5KE30A**, not the SMD SMAJ30A. **(5)** The Mean Well PSU **mounts outside** the enclosure on its own bracket. **(6)** Enclosure is an **opaque grey Boxco P-series** with DIN lever blocks; build is hybrid (bare FETs + socketed ATtiny on protoboard, ×2 relay modules). The AC master switch (DEC-019) is **optional**.
- **Phone-only operator interface (DEC-019).** The on-box panel is cut: **TM1637 4-digit display, 3 momentary buttons, 3 button LED rings — all removed.** The SPA over the device's own Wi-Fi/SoftAP is the sole interface (start/stop/fault-clear/config/calibration). Kept: a single **alive LED** (DevKitC onboard, GPIO2, ~1 Hz blink = firmware ticking). **Why:** the panel is fabrication + parts + enclosure cutouts that don't fit the deploy date, and the SPA already does every panel job. **Physical kill:** a new **AC master switch on the Mean Well input** = whole-system off → fail-dry (no firmware), doubling as the service disconnect / phoneless emergency stop. **Power:** the 24 V LED-ring load and its driver leave the budget (the 12 V buck was already dropped). Reverses the v1.1 "on-box display" addition; the firmware modules are git-recoverable if a panel is ever resurrected.

**Changelog (v1.4):**
- **Corrects the v1.3 valve framing (wrong part).** The as-sourced valve is a U.S. Solid 3/4" brass **2-wire auto-return, normally-closed** motorized ball valve (9–36 V AC/DC, ~2 W, 6–10 s, **capacitor** return) — **on/off, not reverse-polarity**. So it drives off a **single low-side FET** (1 GPIO), not an H-bridge. Every actuated valve is this family; **all DRV8871 H-bridges and the never-both-high invariant are gone.** See DEC-011.
- **Master valve removed.** Fail-dry is now **source control**: the pump sits on the armed 24 V (ATtiny safety relay, DEC-003), powered only during a run → no run / hang / trip / power loss = no pump power = no water. The master's only unique job (block a gravity siphon through a stuck-open zone) doesn't apply here — negligible tank head + the SEAFLO's internal checks block reverse flow. The valves are **not** a safety barrier; their resting-closed state is convenience. See DEC-012.
- **3-way diverter → two 2-way valves** (DEC-013): a **NO** clean/bypass leg + an **NC** Dosatron leg + a check valve on the Dosatron outlet. Unpowered rest = plain water flows, Dosatron isolated; no deadhead.
- **Driver / clamp / rails:** IRLZ44N FET per valve; **SMAJ30A TVS** drain-to-source per FET (internal-rectifier valves — a freewheel diode won't clamp). Valves on **raw 24 V**, pump on **armed 24 V**, **24 V LED rings**, **12 V buck dropped**. Adds DEC-014 (auto-return self-test — correctness only) and DEC-015 (flow-check web override).

**Changelog (v1.3):**
- Zone valves changed to **U.S. Solid 3/4" brass 2-wire reverse-polarity motorized ball valves** (~$34 ea), sitting **downstream of one regulator per tunnel** at ≤15 psi. Replaces the Hunter PGV diaphragm + 458200 latching-solenoid, upstream-of-regulator scheme. Driven by it: the 5-zone Green tunnel needs only one regulator, and **zones + diverter are now one valve family on identical DRV8871 H-bridge channels** (the master stays a de-energize-to-close NC solenoid — see §4). See DEC-011.
- Zone valve rail is no longer a 12V latching pulse: the ball valves run on the **24V rail**, driven for full travel (~5–15 s) like the diverter, not a 75 ms pulse. Firmware impact (`ValveDriver` pulse→travel rename, `PULSE_MS`→`ZONE_TRAVEL_MS`) is tracked as a task in `PROJECT_PLAN.md`.
- **Doc reconciliation:** §2 and §10 still named the superseded SEAFLO **42** pump and an "optional" accumulator, while §4 had already selected the **51** (5.5 GPM, accumulator *required*). Corrected the stale references to match §4. The 42→51 upgrade itself predates this changelog and was never logged on its own.

**Changelog (v1.2):**
- Power source changed from solar bank to a **fixed AC→24V supply installed in the tunnel** (Mean Well LRS-150-24). Decouples Tinkle from the Red solar build; solar now optional/future on the same rail.
- Accumulator: plumb a capped tee, run bare, add a tank only if the pump chatters.
- Driver resolved: **dedicated DRV8871 per valve** (no mux). Watchdog resolved: **ATtiny85**.
- Flow-sensor calibration added to firmware (stored K-factor, web-UI calibration mode).
- Enclosure must now house the open-frame 24V supply.

**Changelog (v1.1, sourcing pass):**
- Pump selected: SEAFLO 42 Series 24V. Accumulator downgraded to optional (pump's internal bypass).
- Master valve selected: industrial direct-acting 24VDC NC, continuous-duty coil (WIC 2BCW).
- Zone valves selected: Hunter PGV diaphragm + Hunter 458200 DC-latching solenoid (12V pulse rail).
- Water path reordered: valves moved **upstream of the pressure regulator** so the diaphragm valves see full pump pressure. Flow sensor placed after master, before zone split.
- Added: automatic Dosatron diverter (motorized 3-way valve + manual isolation + unions for removal).
- Added: TM1637 4-digit LED for MM:SS countdown (read-only). On-box display moved out of "out of scope."

---

## 1. Purpose & Philosophy

Tinkle is an automated drip-irrigation controller for Bay Branch Farm's high tunnels. It replaces the current single 110V smart-plug-on-the-pump setup (pump on/off, no zone control) with scheduled, per-zone watering and reliable manual override.

Design principles, inherited from the Soundings discipline and adapted for a system that **actuates** rather than just observes:

- **Fail dry.** A missed watering cycle is harmless. A valve stuck open overnight drains the tanks, floods a bed, and dead-heads or dry-runs the pump. The entire architecture is built around preventing the *runaway-on* failure, not the missed-cycle failure.
- **Local autonomy.** Watering never depends on the network, a server, or the cloud. The controller runs its schedule from local flash and is configured over local Wi-Fi.
- **Build for three, populate one.** The controller, driver, and enclosure are sized for an eventual three-tunnel farm. Only the Red Tunnel is wired and populated now. Expansion is "land more wires," never a redesign.
- **Scheduled now, closed-loop later.** V1 runs on time and duration only. Soil-moisture-driven irrigation (Soundings sensors driving valves) is explicitly V2, after both the sensors and the valves have each earned a season of trust.

---

## 2. System Architecture

**Two independent paths, one brain** (at full build). Each tunnel gets its own pump, valves, accumulator, and flow sensor. A single controller sequences all of them. V1 builds only the Red path.

**Water path (Red, V1):**

```
Rainwater tanks (west)
   → SEAFLO 55 pump (24V, self-priming, pressure switch, internal bypass, no reverse flow)
   → [ union / camlock — pump serviceable ]
   → accumulator (expansion tank — tames the 5.5 GPM-vs-1.78 GPM short-cycle)
   → filter (100–140 mesh)  → [ union / camlock — filter serviceable ]
   → TEE ─┬─ clean leg:  NO ball valve ───────────────────────────────┐
          └─ fert  leg:  NC ball valve → Dosatron D14MZ2 → check valve ─┤
                 (manual isolation valves + unions on both legs)        → MERGE
   → flow sensor (hall-effect)
   → pressure regulator (one per tunnel, ≤15 psi for tape)
   → [ Zone 1 NC ball valve → beds 1–3 ]
   → [ Zone 2 NC ball valve → beds 4–6 ]
   → AquaTraxx drip tape
```

**Zone valves sit downstream of the regulator, and there is no master valve.** The U.S. Solid auto-return ball valves seal at any pressure, so they live on the regulated ≤15 psi side after a *single* regulator per tunnel (the v1.1 Hunter PGV diaphragm valves needed ~20+ psi and had to sit upstream, which forced a regulator on the combined output). One regulator ahead of the zone split serves every zone — decisive for the 5-zone Green tunnel. **The master valve is gone (DEC-012):** the pump, on the watchdog-armed 24 V, is the source gate — no pump power, no water — so a separate gating valve is redundant here (negligible tank head + the pump's internal checks mean no siphon path with the pump off).

The Red header sits at the **east end** of the tunnel, where the entrance, manual shutoffs, and Dosatron already live. The controller mounts here too, so all Red valves and the flow sensor are within a few feet of the controller — **V1 wiring is short and simple.** (Long buried runs are a future-tunnel concern only.)

**Control path:**

```
ESP32 controller (east end of Red)
   ├── Wi-Fi → existing farm mesh (local web UI; MQTT later)
   ├── low-side FET ×N        → zone valves (NC ball, energize-to-open, ~6–10 s travel)
   ├── low-side FET ×2        → diverter legs (NO clean + NC fert, energize-to-actuate)
   ├── pump-enable output     → pump power on the armed 24V (pump self-manages via pressure switch)
   ├── flow-sensor input      → hall-effect pulse counter
   ├── alive LED (GPIO2)       → ~1 Hz blink = firmware ticking (DEC-019; no on-box panel)
   ├── phone (SPA over Wi-Fi)  → the sole interface: start/stop/fault-clear/config/calibrate
   └── independent hardware watchdog → arms the safety relay feeding the pump (fail-dry source gate)
```

---

## 3. Hydraulic Design (the load-bearing numbers)

**Red Tunnel tape:** Toro Aqua-Traxx **EA5101245** — 5/8" ID, 10 mil wall, 12" emitter spacing, **0.45 GPM per 100 ft**, max pressure 15 psi.

**Geometry:** 6 beds, 66 ft each, 2 tapes per bed.

| Unit | Tape length | Flow @ 0.45/100' |
|---|---|---|
| Per bed | 132 ft | 0.59 GPM |
| Zone (3 beds) | 396 ft | **1.78 GPM** |
| Whole Red (6 beds) | 792 ft | 3.56 GPM |

**Largest single zone = 1.78 GPM.** This is the only number that sizes the pump, because zones run one at a time, never together.

**Verified in the field:** A 7-minute all-beds run at 2% (1:50) injection drew ~1 inch (~0.5 gal) from a 5-gallon concentrate bucket — within tolerance of the predicted 25 gal water / 0.5 gal concentrate. This confirms the tape flows at its rated 0.45, the Dosatron holds 2%, and the 3.56 GPM whole-Red figure is real.

**Dosatron D14MZ2** operating envelope: 0.2–2% (1:500–1:50), flow **0.04–13.2 GPM** (10 L/h–3 m³/h), pressure 4.4–87 psi (0.3–6 bar). Our zones (1.78 GPM) sit comfortably inside this. **The Dosatron is not a design constraint.**

---

## 4. Water-Side Hardware

All external parts: **IP65+ and UV-stable.** This system lives outdoors in full sun.

**Pump** — SEAFLO 55 Series, 24V (SFDP-055-060-55). **Selected (v1.5 — DEC-020).** Heavy-duty pressure switch + anti-vibration mounts.
- Flow: 7.0 GPM open; zone demand 1.78 GPM. Headroom is deliberate — holds pressure through the Dosatron + filter + regulator without straining at its limit. (The accumulator tames the open-flow-vs-zone-demand short-cycle — even more important with the 55's higher open flow.)
- Pressure: adjustable switch — **set to ~20 psi**, not the max. Tape caps at 15 psi behind the regulator; 20 psi covers filter/Dosatron losses with margin, runs the pump easier, draws less current, and at low pressure the pump delivers near open flow (more effective headroom).
- Continuous-duty, run-dry safe, self-priming, internal bypass, DC native (runs straight off the 24V supply).
- **Mount on its anti-vibration mounts; never hard-plumb the ports.** Use a short flex section on inlet AND outlet (below) — the plastic ports are glass-filled nylon and crack when rigid pipe torques or vibrates them. A prior SEAFLO cracked its output this way.
- A prior 51 (cracked output) is on the shelf — bench mule for board/valve/flow testing, not deployment.
- Rejected: model 42 (3.0 GPM, no margin). Superseded: model 51 (5.5 GPM) — the prior pick, replaced by the 55's heavier-duty pressure switch + anti-vibration mounts (DEC-020).

**Flex connectors** — short flexible section on pump inlet and outlet. **Required.**
- Braided stainless line or reinforced tubing, ~12–18". Decouples the rigid plumbing/vibration from the pump's plastic ports.
- Support the pipe weight independently so the plumbing reaches slackly *to* the pump, not hanging *off* it.

**Accumulator / expansion tank** — **required.** Reuse the shelf tank.
- With the 51's 5.5 GPM open flow against a 1.78 GPM zone, the flow mismatch is large — the pump would short-cycle hard without a tank. Field experience confirms this setup needs one.
- The white pre-charged expansion tank from the old Green Tunnel build (on the shelf with the dead 51) is the part — already sized, already owned.
- Mount at the pump output ahead of the filter.

**Pressure regulator** — one per tunnel, **upstream of the zone valves**, ≤15 psi out, 3/4".
- Drops pump pressure to the tape's ≤15 psi *before* the zone split, so both the zone valves and the tape sit at low pressure.
- **One regulator per tunnel, not one per zone** — the motorized ball valves seal fine at low pressure (unlike the diaphragm valves they replace), so a single regulator ahead of the split serves every zone. Decisive for the 5-zone Green tunnel; unifies Red + Green on one design.
- Reuse existing regulator if rated; else Senninger PRL.

**Filter** — 100–140 mesh, reuse existing. Sits before the flow sensor and tape to protect both.

**Dosatron automation** — two motorized 2-way valves split the fert/plain choice (DEC-013), replacing the old 3-way.
- **NO (normally-open) ball valve on the clean/bypass leg** + **NC (normally-closed) ball valve on the Dosatron leg** — both the same U.S. Solid auto-return family as the zones, each on its own low-side FET.
- **Unpowered rest = plain water flows, Dosatron isolated** (NO open, NC closed): exactly one path open, so no both-closed deadhead and no accidental fertigation. A **plain run powers nothing**; a fert run energizes the NC leg open + the NO leg closed.
- **Check valve on the Dosatron outlet** (existing GASHER 3/4" brass, 200 psi), between the injector and the rejoin tee — stops bypass flow back-feeding the idle injector.
- **Manual isolation valves** on both legs for priming/service/winterizing; **unions or camlocks** each side of the Dosatron (and at the pump + filter) so the serviceable string breaks apart.
- Fail-mode: any diverter state (both-open, both-closed, wrong leg) is at worst a fertigation oops, never a flood — it sits upstream of the source gate, and no water moves unless the pump is powered. A leg switch briefly overlaps during the ~6–10 s travel (harmless: pump rides its internal bypass; brief split just under-injects); firmware may stagger make-before-break (optional).

**No master valve (DEC-012).** Earlier revisions used a direct-acting NC solenoid (WIC 2BCW) as a global shutoff. It's removed: the **pump on the watchdog-armed 24 V is the source gate** — no pump power, no water — and with NC zones, a reverse-checking pump, and negligible tank head, a separate gating valve guards nothing the source gate doesn't already. See §6 and DEC-012.

**Zone valves** — U.S. Solid 3/4" brass **2-wire auto-return, normally-closed** motorized ball valve, 9–36 V AC/DC, standard port, NPT. ×2 (Red Tunnel) + ×1 (Zone 3 hose outlet, wired-now/plumbed-later). **Selected (v1.4 — supersedes the v1.1 Hunter scheme and the v1.3 reverse-polarity mis-spec).**
- **NC on/off:** closed when de-energized, energize (24 V) to drive open; a **capacitor auto-return** drives it closed on de-energize. ~2 W, 6–10 s travel. Switched by a single low-side FET (1 GPIO) — no H-bridge, no reverse polarity.
- **Downstream of the regulator:** seals fine at the regulated ≤15 psi.
- **Zone 3** is a general-purpose hose outlet, *separate* from the Red Tunnel's two zones (DEC-006): a third NC valve on its own FET. The board drives it now; the valve is installed when that hose line is plumbed. (This retires the DEC-007 Z3 H-bridge/strapping-pin allocation — a single FET needs only 1 GPIO.)
- **Not a safety barrier (DEC-012):** the NC resting state is a convenience — a valve passes nothing with the pump unpowered. The auto-return is capacitor-driven (needs ~1 min to charge after opening, ages over years), so the self-test (DEC-014) verifies valves rest closed for *correctness*, not fail-dry.
- Red: 2 valves. Green (future): 5 valves. Source: ussolid.com (3/4" brass NC 2-wire auto-return; the NO variant covers the clean diverter leg). Bench-confirm travel time before trusting `ZONE_TRAVEL_MS`.

**Flow sensor** — **Leridian 3/4" NPT hall-effect**, brass, ×1 (Red). Range 2–45 L/min, 253 psi; mounted **after the filter** (after the diverter merge, before the zone split — one sensor reads whichever zone runs). **Selected (v1.5 — DEC-020).** Note: it **won't register flow below 2 L/min**; our zone runs ~1.78 GPM ≈ 6.7 L/min, comfortably above the floor.
- Cross-check: detects flow when nothing is commanded (stuck valve / burst) and no-flow when a zone is on (clog / dead pump). A web-UI override (DEC-015) can mute its *faults* so a bad sensor can't block watering — software only, never touches the source gate.
- **Calibrate empirically** — run a known volume into a bucket, count pulses, derive pulses/gallon. The seed/default K-factor is the Leridian's; don't trust the datasheet K at our ~1.78 GPM (low third of the sensor's range) — recalibrate in the field regardless.
- The irrigation equivalent of the Watermark: confirms reality matched intent.

---

## 5. Control Hardware

**Controller:** ESP32.
- Wi-Fi to existing farm mesh (coverage confirmed at Red; add a mesh node if ever needed).
- Fed from the in-tunnel 24V supply (see Power, below); bucked to 3.3V on-board.
- Mounted at the east end of Red in a weatherproof enclosure.

**Valve driver:** built for the three-tunnel future, populated for Red.
- **Valve channels — one low-side N-FET per valve** (IRLZ44N), gate resistor + gate-to-GND pulldown so each valve sits **off** (closed/rest) through ESP32 boot. **No H-bridge, no DRV8871, no never-both-high invariant** — the valves are on/off. A **TVS (1.5KE30A, through-hole) drain-to-source per FET** clamps the turn-off transient (the valves have an internal bridge rectifier, so a freewheel diode across the valve won't clamp). Build ~8–16 channels; **populate 5** for Red — Z1/Z2/Z3 (zones) + the NO clean leg + the NC Dosatron leg. (Inrush is well within the FET; the cap-inrush spec is moot.)
- **Pump-enable channel:** relay (clean isolation for the ~5A pump) or MOSFET on pump power, on the **armed 24 V** (the fail-dry source gate). Build **3** (one per tunnel); populate **1**.

**Independent watchdog** — non-negotiable, separate from the ESP32.
- **ATtiny85**, separate from the ESP32: watches an ESP32 heartbeat *and* enforces a hard max-runtime ceiling. Its output (the safety relay) sits in series with **pump** power — trip it and the pump de-powers → no source → no water. Fail-dry cutoff in hardware (DEC-012), regardless of what the ESP32 believes.
- This is the automated stand-in for the human who used to watch the hose.

**Manual interface — phone-only (DEC-019):**
- **No on-box controls.** The three zone buttons, the three LED rings, and the TM1637 countdown display are all cut. The **SPA over the device's own Wi-Fi/SoftAP** is the sole interface: start/stop a manual run, clear a fault, edit the schedule, calibrate — everything the buttons + page used to split. (Original panel: 3 IP67 buttons + rings + TM1637, DEC-006 — git-recoverable if ever resurrected.)
- **Physical kill:** an **AC master switch on the Mean Well input** (or unplug) cuts the whole system → fail-dry, purely electrical, no firmware. It is the service disconnect *and* the phoneless emergency stop — a superset of the old "any button stops."
- **On-box status:** a single **alive LED** (DevKitC onboard, GPIO2) blinks ~1 Hz to show the firmware is ticking. Read-only, gates nothing, and distinct from the ATtiny heartbeat. Run state / countdown / faults all live on the SPA status screen.
  - *Hard line: phone-only. Do not re-add a panel as "convenience" — that's a deliberate V1.5 cut (DEC-019).*

**Power:** **fixed AC→24V DC supply, installed in the tunnel** (mains-fed). Replaces the earlier solar-bank dependency for V1 — Tinkle no longer waits on the Red solar build. Solar can later supplement or replace the supply on the same 24V rail; no redesign.
- Candidate: Mean Well **LRS-150-24** (24V, 6.5A / 150W) — covers the pump's peak with margin. LRS-100-24 (4.5A) is marginal against the pump; size up.
- **IP rating:** LRS series is open-frame **IP20** — but it now **mounts *outside* the enclosure** on its own bracket (shaded/vented), not crammed inside the sealed box (DEC-020). (IP67 potted alternative: Mean Well LPV-150-24, if a sealed standalone is preferred.)
- Confirm mains reaches the east-end header (the HAF fan GFCIs suggest AC is present in the tunnel).
- Rails off the 24V supply:
  - **24V (armed, via the safety relay)** → pump. The watchdog gates this — the fail-dry source gate (DEC-012).
  - **24V (raw)** → the valve FETs (zones + both diverter legs; valves are 9–36V). (The 24V button LED rings that used to share this rail are gone — DEC-019. No 12V buck either.)
  - **24→5V buck** → flow sensor **and the ESP32** (fed 5V on 5V/VIN; the ESP32's onboard regulator makes 3.3V, and the 3V3 pin sources the logic rail). **There is no separate 24→3.3V buck** (DEC-020). Level-shift the sensor pulse down to 3.3V for the GPIO.
- Inline fuse (~10A) on 24V output, TVS across 24V, reverse-polarity protection, a per-FET TVS (**1.5KE30A, through-hole**) on each valve channel, flyback on the pump-relay coil (brownout insurance — mains here is brownout-prone).
- **Fail-dry holds on mains loss:** supply dies → pump loses power → no source → no water. A brownout = missed cycle, harmless by design.
- **Build/test:** the same fixed supply powers bench bring-up before tunnel install.

**Wiring (Red, V1):**
- All valves/sensor are at the east-end header, feet from the controller — short runs.
- Where any run is buried or exposed: direct-burial / UF irrigation cable, every splice in a gel- or silicone-filled waterproof connector. No bare wire nuts. (Moisture ingress is the classic failure — same lesson as the HAF fan GFCIs.)
- Conductor count, Red: 3 zone valves (Z1/Z2/Z3) ×2 + 2 diverter legs ×2 + flow ×3 + common. Pull burial cable with spares.

---

## 6. Failure-Mode Design

| Failure | Result | Why it's safe |
|---|---|---|
| Mains loss | Pump dead → no source (valves also rest closed) | No water. Source gone. |
| Controller/firmware hang | Watchdog opens the safety relay → pump de-powered | No runaway-on; valve state irrelevant to dryness. |
| Watchdog trip (time ceiling) | Safety relay opens → pump off → no source | A run can't exceed `HARD_MAX_RUNTIME`. |
| Zone valve stuck open | Between runs the pump is unpowered → no source → no flow; during a run, a bounded amount | Source-gated; runs time-bounded; flow sensor flags idle flow. |
| Pump relay welded on | Pump only has power while the safety relay is armed (during a run); idle = no power | Safety relay sits in series ahead of the enable. |
| Lost Wi-Fi / server | Schedule still runs from flash | Local autonomy. |
| Clog / burst | Flow sensor flags mismatch (mute-able, DEC-015) | Detected, logged. |
| Diverter wrong leg / both open | Wrong fert state | Agronomic oops, not a flood — upstream of the source gate. |

The enemy is *runaway-on*. The **pump-power gate (DEC-012)** — armed only during a run, killed by the watchdog or any power loss — is the single barrier that makes it impossible. The valves decide *which* beds get water during a bounded run, not whether water runs away.

---

## 7. Firmware Behavior

- **Schedule:** stored in flash (time + duration per zone). Runs locally with no network dependency.
- **Sequencing:** zones run one at a time within a tunnel. Open the active zone valve, then pump-enable on; the pump self-manages run/stop via its pressure switch. There is no master valve (DEC-012).
- **Fertigation:** each scheduled run carries a **fertigate flag**. On a fert run, energize the Dosatron leg open + the bypass leg closed before starting the pump; otherwise leave both diverter legs at rest (plain water flows). Default policy: one fert run/day even when multiple watering runs fire.
- **Flow override + self-test:** the flow-sensor faults are mute-able from the web UI (DEC-015, software-only); the firmware periodically verifies valves rest closed for agronomic correctness (DEC-014).
- **Manual:** phone (SPA) → timed run at stored default duration → auto-stop; STOP ALL cancels. The AC master switch is the phoneless whole-system kill (DEC-019).
- **Status:** the SPA status screen shows run state, active zone, and the MM:SS countdown; on the box, a single alive LED blinks ~1 Hz (DEC-019).
- **Flow-sensor calibration:** pulses-per-gallon stored in flash, set via a **calibration mode** on the web UI — start a run into a known container, enter the measured volume, firmware divides counted pulses by volume and saves the K-factor. No reflash to recalibrate. Default seeded from datasheet, overwritten by the field value.
- **Watchdog:** firmware cooperates with the independent hardware watchdog; max-runtime ceiling enforced in hardware regardless of firmware state.
- **Config:** minimal local web page served by the ESP32 over Wi-Fi — set schedules, per-run fert flag, default manual durations, max-runtime. Phone joins Wi-Fi, edits in the field.
- **Telemetry (V1):** local only. Flow and state visible on the web UI.
- **Telemetry (later):** add MQTT publishing to the existing Soundings stack (Mosquitto → TimescaleDB → Grafana). Irrigation events land in the same database as soil-moisture and yield records — making the soil-moisture-vs-fertigation query trivial. The local web UI remains as the field-edit path and last-resort interface.

---

## 8. Expansion Provisions (build now, populate later)

- Valve driver: ~8–16 low-side FET valve channels (5 populated: Z1/Z2/Z3 + NO clean leg + NC Dosatron leg), 3 pump-enable (1 used), 3 flow inputs (1 used). No H-bridges, no master channel.
- No on-box panel (DEC-019, phone-only): the 3 button + LED-ring footprints and the TM1637 are dropped; the freed GPIO bank toward future zones. A single alive LED (GPIO2) remains.
- Enclosure sized for full three-tunnel wiring and terminal blocks. The open-frame LRS mounts **outside** the enclosure on its own bracket (DEC-020), so the box itself only houses the controller/driver/terminals.
- Adding a tunnel = mount a pump + valves + sensor, pull wire home to the controller, populate channels, update config. No board or firmware redesign.

---

## 9. Out of Scope for V1

- Green Tunnel and any third tunnel hardware (chassis headroom only).
- Closed-loop / sensor-driven irrigation (V2).
- Server, MQTT broker, database, Grafana (added with Soundings).
- Second Green irrigation tank (separate decision, pinned — Green is chronically water-limited in summer; tank justified on volume grounds but parked for now).

---

## 10. Open Items — Sourcing Checklist

These are the specs to nail during sourcing. Several feed the driver design, so resolve valves and pump first.

1. ~~**Pump**~~ — **DONE (v1.5, DEC-020):** SEAFLO 55 Series 24V (SFDP-055-060-55), continuous/run-dry, 7.0 GPM, switch set ~20 psi, internal bypass, heavy-duty pressure switch + anti-vibration mounts. (Supersedes the v1.3 model-51 pick, which superseded the v1.1 model-42 — see §4.)
2. ~~**Accumulator**~~ — **DONE (required, §4):** reuse the shelf expansion tank; the 55's 7.0 GPM open against a 1.78 GPM zone short-cycles hard without it.
3. ~~**Zone valves**~~ — **DONE (v1.4):** U.S. Solid 3/4" brass **2-wire auto-return, NC** motorized ball valve, 9–36V, NPT, standard port (2-indicator-light SKU). On/off via a low-side FET. Bench-confirm travel time before trusting `ZONE_TRAVEL_MS`.
4. ~~**Master valve**~~ — **DROPPED (DEC-012):** no master; the pump on the armed 24V is the source gate.
5. ~~**Flow sensor**~~ — **DONE (v1.5, DEC-020):** Leridian 3/4" NPT brass hall sensor (2–45 L/min, 253 psi), mounted after the filter; won't register below 2 L/min (our zone ~6.7 L/min). Seed K = Leridian's; calibrate pulses-per-gallon empirically regardless.
6. ~~**Diverter**~~ — **DONE (DEC-013):** two 2-way US Solid auto-return valves — **NO** clean leg + **NC** Dosatron leg — + the existing GASHER check valve on the Dosatron outlet. Unions/camlocks each side of the Dosatron (+ pump, filter).
7. ~~**Valve driver**~~ — **DONE:** discrete IRLZ44N low-side FET per valve + **1.5KE30A (through-hole) TVS** drain-to-source. No H-bridge. Bare FETs + socketed ATtiny on an ElectroCookie protoboard; ×2 relay modules (pump + safety).
8. ~~**Watchdog**~~ — **DONE:** ATtiny85, safety relay in series with **pump** power (the source gate).
9. **ESP32 board variant** (DevKitC 38-pin candidate) and GPIO budget against channel count (DEC-019 freed 8 GPIO by cutting the panel — budget is comfortable).
10. ~~**Enclosure**~~ — **DONE (v1.5, DEC-020):** opaque grey **Boxco P-series** (~170×220×100), UV-stable, mounts vertical, glands down, DIN rail inside. The 24V PSU mounts **outside** on its own bracket (not in the box). DIN **lever blocks** with jumper bars for the 24V+/GND rails.
11. **Buck converters:** a **single 24→5V module** (feeds the flow sensor + the ESP32; the ESP32 makes its own 3.3V). No 24→3.3V buck, no 12V buck (DEC-020 / DEC-019).
12. ~~**IP67 buttons** with LED rings, ×3. **TM1637 4-digit display**, ×1.~~ — **dropped (DEC-019, phone-only).** Only the DevKitC onboard alive LED (GPIO2) remains; no added BOM.
13. **Burial wire / waterproof connectors:** 18 AWG, conductor count per §5, gel-filled splices, with spares.
14. ~~**Power dependency**~~ — **RESOLVED:** fixed AC→24V supply in the tunnel (Mean Well LRS-150-24 candidate), not the solar bank. Confirm mains reaches the east-end header.

---

*Working name: Tinkle. Revisit before the repo is created.*
