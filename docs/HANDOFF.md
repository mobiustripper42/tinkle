# Tinkle — Hardware Handoff Package (V1)

**Purpose:** a self-contained review package for a second set of eyes. Tinkle is an
ESP32-based drip-irrigation controller for a Bay Branch Farm high tunnel. This doc
synthesizes the canonical repo sources; where those conflict or a value is unconfirmed, it
says so (§9). Canonical sources: `docs/tinkle_v1_spec.md` (hardware), `docs/tinkle_firmware_spec.md`
(behavior), `docs/tinkle_wiring.html` + `src/esp32/pins.h` (pins), `docs/DRAFT-v1.4-BOM.md`
(BOM), `docs/DECISIONS.md` (rationale), `docs/tinkle_buildup.md` (build + netlist).

**Design north star — "fail dry":** the whole architecture exists to prevent *runaway-on*
(a valve/pump stuck watering), not the missed cycle. A missed watering is harmless; an
overnight flood dead-heads the pump and drains the tanks. Every safety decision follows from
this.

---

## 1. System overview

- **What it does:** scheduled, per-zone drip irrigation with reliable manual override and
  automatic fertigation (Dosatron injector diverter). Replaces a dumb smart-plug-on-the-pump.
- **Zones / valves:** **5 motorized ball valves** total — Zone 1, Zone 2 (Red tunnel drip),
  Zone 3 (general hose outlet, wired-now/plumbed-later), plus a 2-valve diverter (NO clean
  leg + NC Dosatron/fert leg). One zone runs at a time (single-active invariant). Board and
  enclosure sized for 3 tunnels; only 1 populated.
- **Pump:** SEAFLO 55 Series diaphragm pump, `SFDP-055-060-55`, **24 V DC**, 7.0 GPM open,
  internal bypass, self-priming, run-dry safe, adjustable pressure switch set ~20 psi.
  **Running current not yet confirmed for the 55** — see §9.
- **Valves:** U.S. Solid 3/4" brass **2-wire auto-return motorized ball valves**. 9–36 V
  AC/DC (run on 24 V), **~2 W (~0.08 A) while open**, 6–10 s travel. **Non-latching** —
  energize to drive open, a capacitor auto-returns them closed on de-energize. On/off only
  (not reverse-polarity), so **one low-side FET per valve, no H-bridge**. NC for zones + fert
  leg; NO for the clean leg. Holding current only while open; inrush negligible vs the FET.
- **Power source:** **mains AC → 24 V DC** via a fixed Mean Well LRS-150-24 (150 W / 6.5 A),
  mounted **outside** the enclosure. A single 24→5 V buck feeds the ESP32 (5 V/VIN → its own
  3.3 V) and the flow sensor. **No battery, no solar in V1** (solar is a future add on the
  same 24 V rail). Mains is brownout-prone here — treated as a fail-dry event, not a fault.

---

## 2. Functional requirements

- **Scheduling:** time + duration per zone, stored in NVS/flash, evaluated once per local
  minute, runs **headless** with no network. Fertigation is a per-run flag; default policy is
  one fert run/day even if multiple watering runs fire. One zone at a time; extra requests
  queue (depth 4).
- **Manual override:** **phone-only** (DEC-019) — a vanilla-JS SPA served by the ESP32 over
  Wi-Fi: start/stop a run, STOP-ALL, clear a fault, edit schedule, calibrate. No on-box
  controls. An **optional** AC master switch on the supply is the physical kill / service
  disconnect.
- **Connectivity:** Wi-Fi only — joins the farm mesh (STA) or falls back to a `Tinkle-Setup`
  SoftAP; mDNS `tinkle.local`. **No Bluetooth, no cloud** in V1. Wi-Fi carries the UI only and
  has **no role in watering** (schedule runs from flash). MQTT telemetry to the Soundings
  stack is a later, non-V1 add.
- **Sensors (V1):** one **hall-effect flow sensor** (Leridian 3/4" NPT). No soil-moisture
  (V2), pressure, rain, or tank-level sensing in V1. Tank-level pump-lockout is a recorded V2
  seam (DEC-017).
- **Failure behavior:**
  | Event | Result | Why safe |
  |---|---|---|
  | Mains/power loss | Pump unpowered; valves rest closed | No source → no water |
  | Firmware hang / crash | Watchdog opens the safety relay → pump dead in ~2 s | Independent of the ESP32 |
  | Run exceeds ceiling | ATtiny hard-max (30 min) or SW ceiling (20 min) stops the run | Time-bounded |
  | Wi-Fi / server loss | Schedule keeps running from flash | Local autonomy |
  | Pump dry-run | Pump is run-dry safe + internal bypass | Pump-tolerant |
  | Stuck-open valve | Between runs pump is off → no flow; flow sensor flags idle flow | Source-gated |
  | Clog / burst / leak | Flow-sensor mismatch flagged + logged (mute-able, DEC-015) | Detected |
  | Welded pump relay | Safety relay (in series, ATtiny-armed) still de-powers at idle | Series gate |

---

## 3. Electrical architecture

- **Controller:** ESP32 **DevKitC (38-pin)** on a screw-terminal breakout (solderless to the
  ESP32). Arduino-ESP32 / PlatformIO.
- **Independent watchdog:** **ATtiny85** (separate binary, runs at 3.3 V), watches an ESP32
  heartbeat and enforces a hard max-runtime; its output arms the safety relay. It commands
  nothing — it can only gate pump power.

**Power tree**

| Rail | Source | Feeds | Est. current |
|---|---|---|---|
| 24 V raw | LRS-150-24 (6.5 A) via 10 A fuse | valve FETs (×5), buck input, relay contacts | ≤ ~0.2 A valves (1–2 open at once) |
| 24 V armed | 24 V raw through **safety relay + pump relay in series** | pump only | pump draw (see §9) |
| 5 V | 24→5 V buck | ESP32 5V/VIN, flow sensor, relay-module coils | ESP32 ~0.15–0.25 A + coils |
| 3.3 V | ESP32 onboard regulator | logic rail (pin-36 pull-up, level-shifter ref) | small |

- **Valve driver (proposed / built):** one **IRLZ44N** logic-level low-side N-FET per valve.
  Gate via 100 Ω from the GPIO; 100 kΩ gate-to-GND pulldown holds the FET **off** through
  boot; a **1.5KE30CA TVS** (bidirectional, as-built) drain-to-source clamps the turn-off transient (the valves have an
  internal bridge rectifier, so a freewheel diode across the valve won't clamp — clamp the
  FET). Valves on **raw 24 V** — a stuck valve passes nothing with the pump off.
- **Pump driver (proposed / built):** **two relays in series** on the pump's 24 V feed — a
  **safety relay** (ATtiny-armed) and a **pump relay** (ESP32-commanded). Both must close for
  the pump to see power. Implemented as **2× 1-channel opto-isolated relay modules**
  (SRD-05VDC-SL-C, 10 A, H/L jumper set HIGH so boot-low = off). Modules carry their own coil
  driver + flyback.
- **Protection:** 10 A inline fuse on 24 V; TVS across 24 V; reverse-polarity (diode/P-FET);
  per-FET 1.5KE30CA TVS; relay-module onboard flyback; gel-filled waterproof splices on every
  outdoor connection (no wire nuts).
- **Isolation:** opto-isolated relay modules isolate the pump-switching side. The ATtiny trip
  line into GPIO36 is **open-drain emulated** (drives low or Hi-Z, never high) so a 5 V-ish
  swing can't overvolt the non-5V-tolerant ESP32 input; an external 10 k pull-up references it
  to 3.3 V. No galvanic isolation is otherwise required (single supply, common ground).

---

## 4. Pin map (ESP32, authoritative — `pins.h` / wiring doc)

Avoids flash pins (6–11) and TX/RX (0/1/3). **No strapping pins in use** — one FET per valve
means no H-bridge, so the old GPIO12/15 boot-mode hazard is retired. All logic is **3.3 V**.

| GPIO | Function | Dir | Pull / passives | Boot-strap concern | Level |
|---|---|---|---|---|---|
| 13 | Zone 1 valve FET gate | OUT | 100 Ω series + 100 k gate→GND | none | 3.3 V |
| 14 | Zone 2 valve FET gate | OUT | 100 Ω + 100 k | none | 3.3 V |
| 16 | Zone 3 valve FET gate | OUT | 100 Ω + 100 k | none | 3.3 V |
| 17 | Diverter clean-leg FET gate | OUT | 100 Ω + 100 k | none | 3.3 V |
| 18 | Diverter fert-leg FET gate | OUT | 100 Ω + 100 k | none | 3.3 V |
| 22 | Pump relay module IN | OUT | (10 k IN→GND advised) | none | 3.3 V |
| 4  | Heartbeat → ATtiny | OUT | 10 k at ATtiny pin7→GND | none | 3.3 V |
| 36 | ATtiny "tripped" ← (SENSOR_VP) | IN | **ext 10 k→3.3 V + 100 nF→GND** | **input-only, no internal pull** | 3.3 V, active-low |
| 27 | Flow-sensor pulse | IN | INPUT_PULLUP, interrupt | none | **needs 5 V→3.3 V level shift** |
| 2  | "Alive" heartbeat LED | OUT | external LED + 330 Ω | this board's onboard LED is power-only (not SW-controllable) — use an external LED | 3.3 V |

Freed by DEC-019 (phone-only) and available for future zones: 25, 26, 23, 32, 33 (output-capable),
34, 35, 39 (input-only), plus 19/21/12/15/5.

---

## 5. Bill of materials (V1, Red tunnel — from `DRAFT-v1.4-BOM.md`)

Substitutions: FET/TVS/relay values are not critical within class; the **valve family, pump
model, flow-sensor model, and PSU** are design-anchored (don't substitute without re-checking
current/pressure). "reuse" = on the shelf.

| Part | MPN / model | Rating | Qty | Notes / sub-OK? |
|---|---|---|---|---|
| Motorized ball valve, NC | U.S. Solid 3/4" brass, 2-wire auto-return NC | 9–36 V, ~2 W, 6–10 s | 3 (+1 later) | Z1, Z2, fert leg (+Z3). Family anchored; NPT standard port |
| Motorized ball valve, NO | U.S. Solid 3/4" brass, 2-wire auto-return NO | same | 1 | clean/bypass leg |
| Pump | SEAFLO `SFDP-055-060-55` | 24 V, 7.0 GPM | 1 | model anchored; deploy unit (dead-51 is a bench mule) |
| Flow sensor | Leridian 3/4" NPT hall | 2–45 L/min, 253 psi, 3.3–18 V | 1 | model anchored; open-collector-style pulse, JST 3-pin |
| ESP32 | DevKitC 38-pin | — | 1 (+spare) | on screw-terminal breakout |
| Watchdog MCU | ATtiny85 (Digispark) | 3.3 V | 1 (+spare) | flash via Arduino-as-ISP |
| Valve FET | IRLZ44N | logic-level, ≫ valve load | 5 | sub-OK: any logic-level low-side N-FET |
| Per-FET TVS | 1.5KE30CA (through-hole, bidirectional) | ~25.6 V standoff | 5 | as-built CA replaced spec'd A — equivalent here; no band, either orientation. Sub-OK within standoff window |
| Relay module | 1-ch opto, SRD-05VDC-SL-C relay | 5 V coil, 10 A, H/L jumper | 2 (4-pack) | **active-HIGH**, 3.3 V-trigger — verify §9 |
| Level shifter | 5 V↔3.3 V module or divider | — | 1 | possibly droppable — §9 |
| 24 V PSU | Mean Well LRS-150-24 | 24 V / 6.5 A / 150 W | 1 | anchored; mounts outside; **6.5 A vs 55 draw — §9** |
| Buck 24→5 V | LM2596-class adjustable | set 5.0 V, ≥1 A | 1 | sub-OK |
| Fuse + holder | blade, 10 A inline on 24 V | 10 A | 1 | — |
| Protection | TVS across 24 V; reverse-polarity diode/P-FET | — | 1 ea | — |
| Enclosure | Boxco P-series `BC-AGP-172210` grey | IP67, ~170×220×100 | 1 | opaque UV-stable |
| Terminals | DIN lever blocks + jumper bars; ElectroCookie protoboard | — | as needed | hybrid solderless/soldered build |
| Field wire | 18 AWG direct-burial/UF | — | + spares | 2 cond/valve + common |
| Splices | gel-filled (King DryConn) | — | as needed | no wire nuts |

> The BOM doc still carries historical panel rows (buttons/TM1637/LED rings — cut by DEC-019)
> and, until PR #112 merges, placeholder relay rows. The rows above reflect the **current**
> design.

---

## 6. Valve and pump details

**Valves — U.S. Solid 3/4" brass 2-wire auto-return motorized ball valve**
- Links: [NC variant](https://ussolid.com/products/3-4-motorized-ball-valve-brass-9-36v-ac-dc-2-wire-auto-return-electric-ball-valve-normally-closed-3-indicator-lights),
  [NO variant](https://ussolid.com/products/3-4-motorized-ball-valve-brass-electric-ball-valve-with-3-indicator-lights-2-wire-auto-return-normally-open-9-36v-ac-dc-by-u-s-solid-html)
- Operating voltage: 9–36 V AC/DC (run at 24 V). Running current: ~2 W ⇒ **~0.08 A** while
  traveling/holding open; **0 A at rest** (NC) — capacitor auto-return. Inrush: not
  characterized but far below the FET rating; treated as moot.
- Travel: 6–10 s (firmware seed `ZONE_TRAVEL_MS`/`DIVERTER_TRAVEL_MS` = 10 s — **bench-confirm**).
- Connector: 2-wire flying leads (+ position-indicator LEDs on the SKU). Cable: short field
  runs, valves at the east-end header a few feet from the controller; direct-burial 18 AWG,
  2 conductors + common per valve, gel-filled splices.
- Environment: **outdoor, wet-location**, full sun, IP65+/UV-stable required.

**Pump — SEAFLO 55 Series `SFDP-055-060-55`, 24 V DC**
- 7.0 GPM open flow (zone demand only 1.78 GPM — headroom is deliberate; accumulator tames
  short-cycle). Internal bypass, self-priming, run-dry safe, adjustable pressure switch ~20 psi.
- **Running / stall / inrush current: NOT yet confirmed for the 55** (the repo's 6.5 A supply
  was sized for the prior 51). See §9.
- Mount on anti-vibration feet; **never hard-plumb** the glass-filled nylon ports — short flex
  connectors on inlet + outlet (a prior unit cracked its output).
- Connector: 2-wire (+ / −) on the armed 24 V. Environment: outdoor/wet.

---

## 7. Firmware behavior

- **Run state machine (`RunController`, sole actuator commander):**
  `Idle → PrepDiverter → OpenZone → StartPump → Running → StopPump → CloseZone → Settle → Idle`,
  plus `Fault`. On boot it forces the safe state (pump off → all valve FETs off → diverter
  plain) and parks at Idle. Any fault from any state → safe state + latch.
- **Timing:** cooperative non-blocking loop, **≤10 ms tick, no `delay()`**. Valve travel timed
  against `millis()` (10 s seed). Inter-run/settle gap 1000 ms. One zone active at a time;
  queue depth 4.
- **Safety interlocks (two-key, fail-dry):** the ESP32 *commands* actuators and emits a
  heartbeat; the ATtiny *arms* the safety relay. Water moves only if both agree. Fertigation
  and zone selection decide *which beds*, never *whether* water can run away — the pump-power
  gate is the single barrier (DEC-012, no master valve).
- **Watchdog (DEC-003/004):** heartbeat square wave on GPIO4 is emitted **only while a run is
  active** ("heartbeat present == run in progress"). ATtiny holds the relay armed only while
  edges arrive within **`HB_TIMEOUT_MS` = 2 s** and armed time < **`HARD_MAX_RUNTIME` = 30 min**
  (its own clock). Software ceiling `swMaxRuntimeSec` = **1200 s (20 min)** fires first in the
  loop. Trip line (GPIO36) is informational/logging; the relay is the real safety.
- **Faults (latched, gated clear):** no-flow, unexpected idle-flow (>`50` pulses idle),
  watchdog, calibration-range, etc. Idle-flow / clog faults are **mute-able from the SPA**
  (DEC-015, software-only — never touches the source gate). Valve auto-return self-test
  (DEC-014) is log-only, not a safety layer.
- **Calibration / config:** flow **K (pulses/gallon)** set via a guided web calibration run
  (run a known volume, enter measured gallons), stored in NVS; seed `1670` for the Leridian
  (flow-dependent, **calibrate empirically**). Configurable: per-zone default durations,
  schedule, fert policy, software max-runtime, Wi-Fi creds. No reflash to reconfigure.

---

## 8. Mechanical / environmental

- **Enclosure:** Boxco P-series grey ABS, IP67, UV-stabilized, ~170×220×100 mm, mounted
  vertical with glands entering the **bottom** face. Houses ESP32 breakout + FET/ATtiny
  protoboard + relay modules + buck + DIN terminals only.
- **PSU:** the open-frame LRS-150-24 is **IP20 → mounts outside** the enclosure on its own
  shaded/vented bracket (keeps its heat out of the sealed box).
- **Target rating:** outdoor, full-sun high tunnel — IP65+ on the box, UV-stable, all external
  parts wet-location. Temp/humidity: high-tunnel summer heat + condensation; direct-burial
  cable, gel-filled splices (moisture ingress is the classic failure mode).
- **Connector locations:** glands on the bottom face; field valve/pump/sensor runs land on DIN
  terminals. Controller mounts at the east-end header, valves/sensor within a few feet (short
  runs; long buried runs are a future-tunnel concern).
- **Mounting:** enclosure wall/post-mounted vertical; PSU on adjacent bracket; ESP32 on nylon
  standoffs; modules DIN-clipped.

---

## 9. Open questions, assumptions, and items needing engineering confirmation

**Needs engineering confirmation (highest priority first):**
1. **PSU vs pump current.** The LRS-150-24 (6.5 A) was sized for the old SEAFLO **51** (5.5 GPM).
   The design moved to the **55** (7.0 GPM, DEC-020) but the supply wasn't re-evaluated.
   *Confirm the 55's running + inrush current; the 6.5 A supply may be marginal and need
   upsizing.*
2. **Relay contact DC rating.** SRD-05VDC-SL-C is rated 10 A at 250 V**AC**; DC ratings derate.
   *Confirm the 24 V DC contact rating covers the pump's actual current (esp. inductive
   inrush), for two contacts in series.*
3. **Relay module 3.3 V trigger.** Modules must be **active-HIGH** and trigger reliably from
   the ESP32's 3.3 V (pump channel). *Bench-verify (tap IN to 3.3 V → click) before trusting.*
4. **Valve travel time.** `ZONE_TRAVEL_MS`/`DIVERTER_TRAVEL_MS` = 10 s is a seed. *Bench-measure
   actual travel + set the constant; the run state machine gates on it.*
5. **Flow K-factor.** Seed 1670 p/gal is a nominal Leridian value (its `F = 8.1·Q − 5` curve is
   flow-dependent). *Calibrate empirically against a measured volume.*

**Assumptions / values Claude derived (not from a datasheet in hand):**
- Valve current ~0.08 A from the "~2 W" spec; inrush "negligible" is an engineering judgment,
  not measured.
- Pump ~5 A "peak" figure in older docs predates the 51→55 change — treat as stale.
- Level shifter may be **droppable**: the Leridian is rated to 3.3 V, so powering it at 3.3 V
  with the ESP32's internal pull-up on GPIO27 could yield a 3.3 V pulse directly. *Unverified —
  confirm the sensor's output type (open-collector vs push-pull) at 3.3 V before removing it.*

**Missing / not-in-repo:**
- No formal datasheets committed for the U.S. Solid valves, SEAFLO 55, or Leridian sensor —
  only product links (§6). A reviewer may want the PDFs.
- The canonical BOM (`DRAFT-v1.4-BOM.md`) is mid-reconcile: it still shows historical panel
  rows and (until PR #112 merges) placeholder relay rows. §5 here reflects the intended
  current state.

**Deliberate scope cuts (so a reviewer doesn't flag them as gaps):**
- Phone-only, no on-box UI/display/buttons (DEC-019). No master valve — pump-power is the
  fail-dry gate (DEC-012). No soil-moisture/closed-loop, no cloud/MQTT, no tank-level lockout —
  all explicitly V2.

---

*Generated from the Tinkle repo as a review aid. Where this doc and a canonical source
disagree, the canonical source (and reality on the bench) wins — flag it and we'll reconcile.*
