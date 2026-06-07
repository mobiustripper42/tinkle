# Tinkle — Irrigation Controller
## Hardware & System Specification, V1

**Status:** Sourcing — water-side parts selected; controller BOM in progress
**Build target:** Winter 2026–27, running the 2027 season
**Scope of V1:** Red Tunnel only, built on a chassis sized for three tunnels

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

**Two independent paths, one brain** (at full build). Each tunnel gets its own pump, master valve, accumulator, and flow sensor. A single controller sequences all of them. V1 builds only the Red path.

**Water path (Red, V1):**

```
Rainwater tanks (west)
   → 24V DC diaphragm pump (SEAFLO 51, self-priming, pressure switch, internal bypass)
   → [ union / camlock — pump serviceable ]
   → accumulator (expansion tank — tames the 5.5 GPM-vs-1.78 GPM short-cycle)
   → filter (100–140 mesh)  → [ union / camlock — filter serviceable ]
   → manual isolation valve → [ union ] → Dosatron D14MZ2 → [ union ] → manual isolation valve
        ⌐ motorized 3-way diverter routes flow THROUGH (fertigate) or AROUND (plain) the Dosatron
   → master valve (WIC 2BCW, direct-acting NC, fail-dry)
   → flow sensor (hall-effect)
   → pressure regulator (one per tunnel, drop to ≤15 psi for tape)
   → [ Zone 1 valve (U.S. Solid motorized ball) → beds 1–3 ]
   → [ Zone 2 valve (U.S. Solid motorized ball) → beds 4–6 ]
   → AquaTraxx drip tape
```

**Zone valves now sit downstream of the regulator (v1.3 flip).** The U.S. Solid motorized ball valves seal at any pressure — zero to full — so they live on the regulated ≤15 psi side, after a *single* regulator per tunnel. The Hunter PGV diaphragm valves they replace needed ~20+ psi to seal and so had to sit upstream on the 45–60 psi pump side, which forced a regulator (or several) on the combined tape output. One regulator ahead of the zone split now serves every zone — decisive for the 5-zone Green tunnel and a unified Red + Green design. The direct-acting WIC master still works at any pressure, so its placement (upstream, gating everything) is unchanged.

The Red header sits at the **east end** of the tunnel, where the entrance, manual shutoffs, and Dosatron already live. The controller mounts here too, so all Red valves, the master, and the flow sensor are within a few feet of the controller — **V1 wiring is short and simple.** (Long buried runs are a future-tunnel concern only.)

**Control path:**

```
ESP32 controller (east end of Red)
   ├── Wi-Fi → existing farm mesh (local web UI; MQTT later)
   ├── H-bridge valve driver  → zone valves (motorized ball, full-travel drive)
   ├── H-bridge valve driver  → Dosatron 3-way diverter (full-travel drive, same as a zone)
   ├── low-side switch         → master valve (NC, holding while open)
   ├── pump-enable output      → pump power (pump self-manages via pressure switch)
   ├── flow-sensor input       → hall-effect pulse counter
   ├── TM1637 4-digit LED       → MM:SS countdown of active run (read-only status)
   ├── 3× momentary button + LED ring (Red live, two spare)
   └── independent hardware watchdog → hard max-runtime cutoff
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

**Pump** — SEAFLO 51 Series, 24V (SFDP2-055-060-51). **Selected.**
- Flow: 5.5 GPM open; zone demand 1.78 GPM. Headroom is deliberate — holds pressure through the Dosatron + filter + regulator without straining at its limit.
- Pressure: adjustable switch — **set to ~20 psi**, not the 60 max. Tape caps at 15 psi behind the regulator; 20 psi covers filter/Dosatron losses with margin, runs the pump easier, draws less current, and at low pressure the pump delivers near open flow (more effective headroom).
- Continuous-duty, run-dry safe, self-priming, internal bypass, DC for the no-inverter solar bank.
- **Mount on its rubber vibration-isolation feet; never hard-plumb the ports.** Use a short flex section on inlet AND outlet (below) — the plastic ports are glass-filled nylon and crack when rigid pipe torques or vibrates them. A prior 51 cracked its output this way.
- A prior 51 (cracked output) is on the shelf — bench mule for board/valve/flow testing, not deployment.
- Rejected: model 42 (3.0 GPM, no margin); model 55 (7.0 GPM, more headroom than needed).

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

**Dosatron automation** — motorized diverter so the once-a-day fert pulse runs unattended.
- **Motorized 3-way ball valve, 24VDC, L-port** (US Solid or equiv) at the Dosatron inlet. Routes flow through the injector (fertigate) or around it (plain water).
- Holds position unpowered; draws current only while rotating (~5–10s). Driven by one DRV8871 H-bridge channel for the full travel window — **identical to a zone valve** (same valve family, v1.3).
- **Manual isolation valves** retained on both Dosatron legs for priming, service, winterizing.
- **Unions (or camlocks) on each side of the Dosatron** so the injector lifts out without disturbing threaded ports. Recommend unions/camlocks at the pump and filter too — whole serviceable string breaks apart.
- Fail-mode: a 3-way holds its last position on power loss. Worst case is "fertigated when plain was intended" — an agronomic oops, never a flood (no water moves unless master + pump are live, both fail dry). Rides safely on top of the existing design.
- *Single 3-way preferred over two 2-way solenoids — avoids the both-closed deadhead risk.*

**Master valve** — WIC 2BCW (or equiv), **direct-acting, normally-closed, 24VDC, continuous-duty coil**. **Selected.**
- Direct-acting → works from zero pressure, so placement is free and the regulated low-pressure side is irrelevant.
- True fail-dry guarantee: power dies → spring closes → no water regardless of any zone valve's state. **This is why the master stays a de-energize-to-close NC solenoid even though the zones and diverter are now motorized ball valves** (v1.3): a ball valve holds its last position on power loss, so it can never be the element that guarantees "dry." The master is the one valve that must spring closed.
- Holding draw ~18W while watering only (pump running anyway). Modest, intermittent, fine on solar.
- **Continuous-duty coil required.** Reject the U.S. Solid general-purpose units here — they're rated under 8 hours energized and warn of coil burnout on continuous duty.

**Zone valves** — U.S. Solid 3/4" brass **2-wire reverse-polarity motorized ball valve**, 9–24V DC, standard port (JFMSV-series), ~$34 ea. ×2 (Red Tunnel) + ×1 (Zone 3 hose outlet, wired-now/plumbed-later). **Selected (v1.3 — supersedes Hunter PGV + 458200).**
- **Zero-pressure operation:** seals at the regulated ≤15 psi, so it sits *downstream* of the regulator (the diaphragm valves it replaces needed ~20+ psi and had to sit upstream).
- Reverses on polarity flip → drives off a DRV8871 H-bridge, the **same channel type and valve family as the Dosatron diverter**. Holds position with no power once moved; self-stops at its internal limit switch.
- **~5–15 s travel — driven for the full travel window like the diverter, not a 75 ms pulse.** Runs on the **24V rail** (no separate 12V pulse rail). Bench-confirm the travel time before trusting the firmware's `ZONE_TRAVEL_MS`.
- **Zone 3** is a general-purpose hose outlet, *separate* from the Red Tunnel's two zones (DEC-006): a third ball valve on its own H-bridge (Z3_IN1/IN2 = GPIO15/12, DEC-007). The board drives it now; the valve is installed when that hose line is plumbed.
- Fail *as-is* (holds last position) on power loss — but cannot pass water unless the master is also open and the pump live. The NC master + watchdog backstop this; the ball valve is deliberately **not** the fail-dry element (it holds position, it does not spring closed — see the master entry below).
- Red: 2 valves. Green (future): 5 valves. ~$238 for all seven vs. 5 regulators + fittings + leak points in Green. Source: ussolid.com (3/4" brass standard port, 2-wire reverse-polarity).

**Flow sensor** — hall-effect, brass, 3/4", ×1 (Red). Placed after the master, before the zone split (one sensor reads whichever zone runs).
- Cross-check: detects flow when nothing is commanded (stuck valve / burst) and no-flow when a zone is on (clog / dead pump).
- **Calibrate empirically** — run a known volume into a bucket, count pulses, derive pulses/gallon. Don't trust the datasheet K-factor at our ~1.78 GPM (low third of the sensor's range).
- The irrigation equivalent of the Watermark: confirms reality matched intent.

---

## 5. Control Hardware

**Controller:** ESP32.
- Wi-Fi to existing farm mesh (coverage confirmed at Red; add a mesh node if ever needed).
- Fed from the in-tunnel 24V supply (see Power, below); bucked to 3.3V on-board.
- Mounted at the east end of Red in a weatherproof enclosure.

**Valve driver:** built for the three-tunnel future, populated for Red.
- **Zone-valve channels:** **dedicated H-bridge per valve** — one DRV8871 (3.6A) wired straight to each motorized ball valve, no multiplexing. Valves drive ~5–15 s for travel and never run together; dedicated bridges are simple to wire and reason about, the chips are cheap, and it's the **same channel type as the diverter**. Build ~16 channels of headroom; **3 channels wired now** — Z1/Z2 (Red, GPIO13/14, 16/17) + Z3 (hose outlet, GPIO15/12 strapping pins per DEC-007). Valves plumbed: 2 (Red) now, Z3 when its line goes in.
- **Dosatron diverter channel:** one more H-bridge channel for the 24VDC 3-way motorized valve. Populate **1** (shared across tunnels at full build, or one per tunnel — TBD).
- **Master-valve channels:** low-side MOSFET switch (logic-level N-FET, e.g. IRLZ44N) for the NC solenoid. Build **3** (one per tunnel); populate **1**.
- **Pump-enable channels:** relay (clean isolation for the ~5A pump) or MOSFET on pump power. Build **3**; populate **1**.

**Independent watchdog** — non-negotiable, separate from the ESP32.
- **ATtiny85**, separate from the ESP32: watches an ESP32 heartbeat *and* enforces a hard max-runtime ceiling. Its output sits in series with master + pump power — trip it and the master loses power and springs closed. Fail-dry cutoff in hardware, regardless of what the ESP32 believes.
- This is the automated stand-in for the human who used to watch the hose.

**Manual interface:**
- **3× IP67 momentary buttons with LED rings** — one per zone, all three live: Z1/Z2 (Red Tunnel) + Z3 (hose outlet). No dedicated stop button (DEC-006).
- Button is an **input to the ESP32**, never wired across a valve — all watering paths stay under master + watchdog control.
- Press while idle = trigger a **timed run** of that button's zone for a stored default duration; the run auto-stops itself.
- **Any** button press during an active run = **stop** (cancel the run; no switch, no auto-start). A **≥3 s long-press of any button** clears a latched fault (DEC-006).
- LED ring = "this tunnel is watering right now." That is the only status needed at the box.
- **LED countdown display:** TM1637 4-digit module (~$3, 2-wire, drives off 2 GPIO). Shows **MM:SS countdown** of the active run via the colon. Read-only status only — config stays on the web page; cannot affect any watering path. 7-segment reads better in sun than LCD behind the clear lid.
  - *Hard line: countdown only. Do not let it grow into an on-box menu/UI.*

**Power:** **fixed AC→24V DC supply, installed in the tunnel** (mains-fed). Replaces the earlier solar-bank dependency for V1 — Tinkle no longer waits on the Red solar build. Solar can later supplement or replace the supply on the same 24V rail; no redesign.
- Candidate: Mean Well **LRS-150-24** (24V, 6.5A / 150W) — covers the pump's ~5A peak with margin. LRS-100-24 (4.5A) is marginal against the pump; size up.
- **IP rating:** LRS series is open-frame **IP20** — must live *inside* the sealed enclosure (or its own sealed box). It does not mount in open tunnel air. (IP67 potted alternative: Mean Well LPV-150-24, if separate mounting is wanted.)
- Confirm mains reaches the east-end header (the HAF fan GFCIs suggest AC is present in the tunnel).
- Rails off the 24V supply:
  - **24V raw** → master valve, pump, and the zone + diverter H-bridge motor supply (the ball valves are 9–24V — run them at 24V; no 12V pulse rail).
  - **24→12V buck** → button LED rings only. (The old zone-valve pulse rail is gone with the move to motorized ball valves; the diverter, formerly listed here, also runs on 24V.)
  - **24→5V buck** → flow sensor (level-shift its pulse down to 3.3V for the GPIO).
  - **24→3.3V buck** → ESP32 + logic.
- Inline fuse (~10A) on 24V output, TVS across 24V, reverse-polarity protection, flyback diodes on every inductive load (brownout insurance — mains here is brownout-prone).
- **Fail-dry holds on mains loss:** supply dies → 24V drops → master springs closed → no water. A brownout = missed cycle, harmless by design.
- **Build/test:** the same fixed supply powers bench bring-up before tunnel install.

**Wiring (Red, V1):**
- All valves/sensor are at the east-end header, feet from the controller — short runs.
- Where any run is buried or exposed: direct-burial / UF irrigation cable, every splice in a gel- or silicone-filled waterproof connector. No bare wire nuts. (Moisture ingress is the classic failure — same lesson as the HAF fan GFCIs.)
- Conductor count, Red: 2 zone valves ×2 + Dosatron diverter ×2 + master ×2 + flow ×3 + common. Pull burial cable with spares.

---

## 6. Failure-Mode Design

| Failure | Result | Why it's safe |
|---|---|---|
| Power loss | Master shuts (NC) | No water. Fail-dry. |
| Controller/firmware hang | Watchdog cuts power past time ceiling | No runaway-on. |
| Zone valve stuck open | No water unless master also open | Two-fault tolerance. |
| Pump relay welded on | Pump pressurizes, but the NC master is closed (and zone valves hold closed) → no path | Single fault ≠ flood. |
| Lost Wi-Fi / server | Schedule still runs from flash | Local autonomy. |
| Clog / burst | Flow sensor flags mismatch | Detected, logged. |
| Dosatron diverter stuck | Wrong fert state (plain vs injected) | Agronomic oops, not a flood — water still gated by master + pump. |

The enemy is *runaway-on*. Every layer above exists to make a single fault unable to flood a bed.

---

## 7. Firmware Behavior

- **Schedule:** stored in flash (time + duration per zone). Runs locally with no network dependency.
- **Sequencing:** zones run one at a time within a tunnel. Pump-enable on for the active tunnel; pump self-manages run/stop via its pressure switch; master opens for the active zone group.
- **Fertigation:** each scheduled run carries a **fertigate flag**. On a fert run, set the Dosatron diverter to THROUGH before opening the master; otherwise leave it AROUND. Default policy: one fert run/day even when multiple watering runs fire.
- **Manual:** momentary button → timed run at stored default duration → auto-stop. Any button = cancel all.
- **Display:** TM1637 shows MM:SS countdown of the active run; blank/idle otherwise.
- **Flow-sensor calibration:** pulses-per-gallon stored in flash, set via a **calibration mode** on the web UI — start a run into a known container, enter the measured volume, firmware divides counted pulses by volume and saves the K-factor. No reflash to recalibrate. Default seeded from datasheet, overwritten by the field value.
- **Watchdog:** firmware cooperates with the independent hardware watchdog; max-runtime ceiling enforced in hardware regardless of firmware state.
- **Config:** minimal local web page served by the ESP32 over Wi-Fi — set schedules, per-run fert flag, default manual durations, max-runtime. Phone joins Wi-Fi, edits in the field.
- **Telemetry (V1):** local only. Flow and state visible on the web UI.
- **Telemetry (later):** add MQTT publishing to the existing Soundings stack (Mosquitto → TimescaleDB → Grafana). Irrigation events land in the same database as soil-moisture and yield records — making the soil-moisture-vs-fertigation query trivial. The local web UI remains as the field-edit path and last-resort interface.

---

## 8. Expansion Provisions (build now, populate later)

- Valve driver: ~16 H-bridge channels (3 wired: Z1/Z2/Z3, 2 plumbed) + Dosatron diverter channel(s), 3 master channels (1 used), 3 pump-enable (1 used), 3 flow inputs (1 used).
- 3 button + LED footprints (1 used). TM1637 display populated for Red.
- Enclosure sized for full three-tunnel wiring and terminal blocks **plus the 24V supply** (the open-frame LRS lives inside the sealed box).
- Adding a tunnel = mount a pump + master + valves + sensor, pull wire home to the controller, populate channels, update config. No board or firmware redesign.

---

## 9. Out of Scope for V1

- Green Tunnel and any third tunnel hardware (chassis headroom only).
- Closed-loop / sensor-driven irrigation (V2).
- Server, MQTT broker, database, Grafana (added with Soundings).
- Second Green irrigation tank (separate decision, pinned — Green is chronically water-limited in summer; tank justified on volume grounds but parked for now).

---

## 10. Open Items — Sourcing Checklist

These are the specs to nail during sourcing. Several feed the driver design, so resolve valves and pump first.

1. ~~**Pump**~~ — **DONE:** SEAFLO 51 Series 24V (SFDP2-055-060-51), continuous/run-dry, 5.5 GPM, switch set ~20 psi, internal bypass. (Supersedes the v1.1 model-42 pick — see §4.)
2. ~~**Accumulator**~~ — **DONE (required, §4):** reuse the shelf expansion tank; the 51's 5.5 GPM against a 1.78 GPM zone short-cycles without it.
3. ~~**Zone valve**~~ — **DONE (v1.3):** U.S. Solid 3/4" brass 2-wire reverse-polarity motorized ball valve, 24V, driven full-travel off a DRV8871 (same family as the diverter), downstream of one regulator. Bench-confirm travel time before trusting `ZONE_TRAVEL_MS`.
4. ~~**Master valve**~~ — **DONE:** WIC 2BCW direct-acting NC 24VDC, continuous-duty coil. Confirm port size + holding current.
5. **Flow sensor:** pick exact brass 3/4" unit; calibrate pulses-per-gallon empirically.
6. **Dosatron diverter:** 24VDC 3-way L-port motorized ball valve — make/model, port size, pulse/drive spec. Unions or camlocks each side of the Dosatron (+ pump, filter).
7. **Valve driver:** H-bridge chip (DRV8871 candidate) + multiplex/select once bench pulse confirmed.
8. ~~**Watchdog**~~ — **DONE:** ATtiny85, in series with master + pump power.
9. **ESP32 board variant** (DevKitC 38-pin candidate) and GPIO budget against channel count + TM1637.
10. **Enclosure:** IP65+, sized for three-tunnel terminal blocks **+ the 24V supply inside**, clear/vented lid, DIN rail + glands.
11. **Buck converters:** 24→12V (button LED rings), 24→5V, 24→3.3V modules.
12. **IP67 buttons** with LED rings, ×3. **TM1637 4-digit display**, ×1.
13. **Burial wire / waterproof connectors:** 18 AWG, conductor count per §5, gel-filled splices, with spares.
14. ~~**Power dependency**~~ — **RESOLVED:** fixed AC→24V supply in the tunnel (Mean Well LRS-150-24 candidate), not the solar bank. Confirm mains reaches the east-end header.

---

*Working name: Tinkle. Revisit before the repo is created.*
