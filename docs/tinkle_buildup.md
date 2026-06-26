# Tinkle — Electrical Build-Up Sequence

> **Scope: electrical only.** Plumbing is covered separately. This is the order in
> which to wire and **power up the controller incrementally**, with a verify-before-you-
> proceed gate at each stage so a wiring fault is caught at the rail it's on — not after
> everything is connected and the smoke is already out.
>
> **Pin authority:** `docs/tinkle_wiring.html` §B (mirrored in `src/esp32/pins.h`). If
> anything here disagrees with the wiring doc on a pin, **the wiring doc wins.** This file
> owns the *order*, not the pinout.
>
> **The golden rule of this build:** *power up one rail / one channel at a time, meter it,
> then move on.* Never energize the whole board on first power-up. The pump goes on **last**,
> because it's the only thing that can actually move water — and the whole architecture exists
> to keep it from moving water when it shouldn't.

---

## Before you start — what you're building toward

Two keys gate the pump, and the build sequence brings them up in dependency order:

- **ESP32 = the command key.** Drives one low-side FET per valve (energize = actuate) and
  the pump-relay trigger (GPIO22). Emits a heartbeat square wave on **GPIO4 only while a
  run is active** (DEC-004 / `pins.h`: "heartbeat present" == "a run is in progress").
- **ATtiny85 = the arm key.** Independent MCU. Holds the **NO safety relay** closed
  (energize-to-pass) only while the heartbeat is alive *and* the run is under its own
  hard max-runtime ceiling (~30 min, its own clock). The safety relay feeds **armed 24V to
  the pump**.

Water moves only when both agree. The **valves run off raw (always-on) 24V**; only the
**pump runs off armed 24V**. That split is deliberate (DEC-012) — the pump is the source
gate, so the valves don't need gating and you avoid a de-arm-vs-close timing race. Wire it
that way; do not "simplify" the valves onto the armed rail.

### Bench setup (do the whole bring-up on the bench first)

The same Mean Well LRS-150-24 powers bench bring-up and the eventual tunnel install
(spec §5). Bench order beats wiring it live in the tunnel.

- **Bench mule, not the deployment pump:** the cracked-output SEAFLO 51 on the shelf is the
  bench pump (§4). Never wet-test with it; it's for "does the relay click and does the pump
  draw current" only.
- **LED stand-ins for valves/pump** through Stage 7 — the Wokwi `diagram.json` already maps
  every output to an LED (alive=2, pump=22, Z1=13, Z2=14, Z3=16, div-clean=17, div-fert=18,
  flow=27). Drive LEDs (or just meter the gate) before you ever connect a 24V valve.
- **Tools:** multimeter (non-negotiable), the LRS, a current-limited bench supply if you
  have one for first ESP32 power-up, the ATtiny programmer (Digispark / AVR-ISP).

---

## Stage 0 — Inventory + the safe-state mental model

**Wire nothing yet.** Confirm you have, per the BOM (`DRAFT-v1.4-BOM.md`) and spec §5:

- [ ] LRS-150-24 PSU, inline fuse (~10A) + holder, TVS across 24V, reverse-polarity diode/P-FET
- [ ] **AC master switch** for the Mean Well *input* (DEC-019 — this is the service
      disconnect *and* the phoneless emergency stop; it is part of V1, not optional)
- [ ] 24→3.3V buck (ESP32/logic), 24→5V buck (flow sensor)
- [ ] ESP32 DevKitC 38-pin, ATtiny85 + programmer
- [ ] 5× IRLZ44N (Z1/Z2/Z3 + div-clean + div-fert), 5× SMAJ30A TVS, gate resistors (~100Ω)
      + gate-to-GND pulldowns (~100k) per channel
- [ ] Pump relay (~5–6A) + **NO safety/arm relay (≥10A)**, 1N4007/Schottky flyback for each
      relay **coil**
- [ ] Flow-sensor 5V→3.3V level shift (divider or module), 10k pull-up + 100nF for the
      WD-trip line (GPIO36)
- [ ] Terminal blocks + DIN rail, common-ground bus

**Safe / rest level is LOW everywhere** (conventions): NC zone valves closed, NC fert leg
closed, NO clean leg open (plain water), pump off. Every FET must sit **off through ESP32
boot** — that's what the gate pulldown buys you. Hold this picture; every stage either
preserves it or is testing a deliberate departure from it.

---

## Stage 1 — The 24V rail (power + protection)

Build the source rail and its protection *before* anything draws from it.

1. AC master switch → Mean Well AC input. **24V output → inline fuse → distribution bus.**
2. On the protected 24V bus: **TVS across 24V**, **reverse-polarity protection** inline.
3. Nothing else connected yet.

**✅ Gate — meter before proceeding:**
- Master switch OFF → bus reads 0V. ON → bus reads ~24V, stable.
- Fuse intact, polarity correct at every downstream tap (mark + and − on the bus).
- Flip the master a few times — clean off/on, no arc-y chatter. This switch is your
  emergency stop; confirm it actually kills the rail.

> Do **not** wire the bucks, ESP32, or any load until 24V is clean and the master switch
> verifiably cuts it.

---

## Stage 2 — Logic rails (the bucks), isolated

Bring up the bucks **with their outputs disconnected from any IC.** Set them, then meter.

1. 24V bus → **24→3.3V buck** input. Leave the output floating (no ESP32 yet).
2. 24V bus → **24→5V buck** input. Leave the output floating (no flow sensor yet).

**✅ Gate — meter the *outputs* before connecting anything:**
- 3.3V buck output = 3.3V ±5%. **If it's adjustable and reads 5V/12V, you'll cook the
  ESP32 — set and confirm it now, with nothing attached.**
- 5V buck output = 5.0V ±5%.
- Both stable across a master OFF→ON cycle.

> A miswired/misadjusted buck is the single most expensive mistake in this build. Meter the
> output voltage with a free output **every time** before an IC sees it.

---

## Stage 3 — Common ground (the star point)

Before connecting logic, establish the single ground reference. This stage is "do it
right," not "power something."

1. Tie together at **one star point**: both buck grounds, the 24V return, and (next stage)
   the ESP32 GND and ATtiny GND.
2. Use a real ground bus / terminal, not a daisy chain through random connectors.

**✅ Gate:**
- Continuity (near-0Ω) from each ground tap back to the star point.
- No ground loop through the enclosure/DIN rail.

> Floating logic ground against a switching 24V rail is the classic intermittent-glitch
> source (wiring doc §D). Get this right once, here.

---

## Stage 4 — ESP32 alone (command key, no loads)

Now power the brain — and **only** the brain. No FETs, no relays, no valves.

1. 3.3V buck output → ESP32 3V3 + GND (to the star point). *(Or power over USB for the very
   first flash — just don't back-feed 3V3 from two sources at once.)*
2. Flash the firmware: `pio run -e esp32 -t upload` (toolchain notes in `CLAUDE-context.md`).
3. Nothing on GPIO 13/14/16/17/18/22/4/27 yet.

**✅ Gate:**
- **Alive LED (GPIO2, onboard) blinks ~1 Hz** = firmware ticking. This is your "the brain
  is alive" signal and it gates nothing — it just has to blink.
- ESP32 joins Wi-Fi (or raises the `Tinkle-Setup` SoftAP); `http://tinkle.local` serves the
  SPA. Status screen loads.
- Serial monitor clean: `pio device monitor -b 115200`. No boot loop, no brownout resets
  (if it resets, suspect the 3.3V buck sag — recheck Stage 2 under load).

> At this point you have a working controller that commands nothing. Every later stage hangs
> one load off a verified-good brain.

---

## Stage 5 — Watchdog + safety relay (the arm key)

Bring up the fail-dry chain **before** anything it protects can move. You want the gate
proven shut before you build the thing it gates.

1. **ESP32 GPIO4 (HEARTBEAT_OUT) → ATtiny heartbeat input.**
2. **ATtiny "tripped" out → ESP32 GPIO36 (WD_TRIPPED_IN).** GPIO36 is input-only, no
   internal pull-up: add the **external 10k to 3.3V + 100nF**. The line is **active-low**
   (idles HIGH via the pull-up; ATtiny drives LOW = tripped, Hi-Z = released — open-drain
   emulation so a 5V ATtiny can't overvolt the 3.3V pin). An *absent* watchdog reads
   "not tripped" — the relay is the real safety.
3. ATtiny arm output → **NO safety relay coil**, **flyback (1N4007/Schottky) across the
   coil**. Relay contacts switch **armed 24V** — but leave the **pump side of those
   contacts disconnected** for now (you're testing the gate, not driving a load).
4. ATtiny GND → star point. Program the ATtiny (separate binary: `pio run -e attiny85`).

**✅ Gate — prove the gate opens *and* closes for the right reasons:**
- No run active → no heartbeat → **safety relay open** → no continuity across the armed-24V
  contacts. Meter it: open.
- Start a manual run from the SPA → heartbeat appears → relay **closes** (click; continuity
  across the contacts; meter the armed-24V output = ~24V).
- **Kill the heartbeat mid-run** (stop the run, or pull GPIO4): relay must **open within
  ~2s**. This is the firmware-hang case.
- Let a run sit (or shorten the ATtiny ceiling for the bench): the **max-runtime trip**
  opens the relay on the ATtiny's own clock, and GPIO36 reflects "tripped" on the SPA/log.
- **Master switch OFF** at any point → relay drops → armed 24V dead. (Power-loss case.)

> Do not proceed until the armed-24V output is **only** live during a healthy, in-bounds
> run and dies on every fault path above. This is the §17 fail-dry behavior in hardware —
> the firmware spec §17 checklist is the definition of done for it.

---

## Stage 6 — One valve channel (prove the FET pattern once)

Build a **single** valve channel end-to-end on **raw 24V**, verify it, then replicate.
Start with Zone 1 (GPIO13).

1. GPIO13 → **~100Ω series gate resistor** → IRLZ44N gate.
2. **~100k gate-to-GND pulldown** at the gate (holds the FET off through boot).
3. IRLZ44N source → ground (star point); drain → load return. **SMAJ30A TVS
   drain-to-source** across the FET (the valves have an internal bridge rectifier, so a
   freewheel diode across the *valve* won't clamp — clamp the FET).
4. Load high side on **raw 24V** (not armed). For the bench, use the LED stand-in or just
   meter the drain; only connect a real valve once the gate behavior is confirmed.

**✅ Gate — confirm rest-state first, then actuation:**
- **At ESP32 boot / firmware idle: FET is OFF** (gate near 0V, drain pulled up = valve would
  be at NC rest = closed). This is the load-bearing check — the pulldown doing its job.
- Command Zone 1 from the SPA → gate goes high → FET on → LED lights / drain pulls low /
  (real valve) travels open ~6–10s and self-cuts at its limit.
- Stop → FET off → (real valve) capacitor auto-returns closed.
- No TVS heating, no gate ringing.

> Get this one channel perfect — gate resistor, pulldown, TVS, boot-off behavior — because
> Stage 7 is just "do the same thing four more times."

---

## Stage 7 — Remaining valve channels

Replicate Stage 6 for each, all on **raw 24V**, each with its own gate R + pulldown + TVS:

| Channel | GPIO | Valve type | Rest (de-energized) |
|---|---|---|---|
| Zone 2 | 14 | NC | closed |
| Zone 3 (hose) | 16 | NC | closed — wired now, plumbed later |
| Diverter clean leg | 17 | **NO** | **open = plain water** |
| Diverter fert leg | 18 | NC | closed (Dosatron isolated) |

**✅ Gate:**
- Each channel boots **off** and actuates only on its own command (verify there's no
  cross-talk — commanding Z2 doesn't twitch Z3).
- **Diverter rest state is correct: clean (NO) open + fert (NC) closed = plain water flows,
  Dosatron isolated.** Exactly one leg open at rest — no both-closed deadhead. A fert run
  energizes fert-open + clean-closed.
- Zone 3 channel works even though no valve is plumbed (build-for-three).

---

## Stage 8 — Pump relay, on the armed rail (LAST)

Only now connect the thing that moves water — and put it where the watchdog can kill it.

1. **GPIO22 (PUMP_RELAY) → pump-relay trigger.** Flyback across the pump-relay coil.
2. Pump-relay contacts switch power to the pump **from the armed-24V output of the Stage 5
   safety relay** — *not* from raw 24V. The safety relay sits **in series ahead of** the
   pump enable.
3. Bench: drive the cracked shelf-51 mule (or an LED/load) — confirm the relay clicks and
   the pump draws current. Do **not** wet-test here.

**✅ Gate — the whole two-key chain, together:**
- Pump runs **only** when GPIO22 commands it **and** the safety relay is armed (run active +
  heartbeat healthy + within max-runtime). Either key missing → pump dead.
- **Kill heartbeat during a pump run → pump de-powers** (safety relay opens upstream of the
  welded-relay case: even a welded pump relay can't keep the pump live, because its supply
  is gone).
- **Master switch OFF → pump dead instantly.**
- Idle (no run) → pump has **no power** (armed rail open).

> If the pump can run with the heartbeat dead, with the run stopped, or with the master off —
> **stop and fix it.** That is the one failure the entire design exists to prevent.

---

## Stage 9 — Flow sensor (sensing, last because it's read-only)

The flow sensor gates nothing (DEC-015 lets its faults be muted), so it comes after the
actuation chain is proven.

1. Flow sensor power from the **5V buck**.
2. Sensor pulse output → **5V→3.3V level shift** (divider or module) → **GPIO27**
   (interrupt, INPUT_PULLUP).
3. **Do not feed the 5V pulse straight to GPIO27** — it cooks the input over time.

**✅ Gate:**
- Bench: a square-wave source (or the Wokwi flow switch on GPIO27/19 loopback) produces a
  pulse count on the SPA / serial.
- The level-shifted pulse at GPIO27 peaks at ~3.3V, never 5V (meter or scope it).
- Calibrate K **empirically later** with real water (run a known volume, count pulses) —
  don't trust the datasheet K at our ~1.78 GPM (low third of range). That's a wet-confirm
  task, not a bench-electrical one.

---

## Stage 10 — Full integration + fail-dry acceptance

Everything connected, on the bench, LEDs/mule standing in for the wet load.

**✅ Final electrical gate — walk the failure table (spec §6) by hand:**
- [ ] **Mains/master loss** → pump dead, valve FETs de-energized (all rest), system dry.
- [ ] **Firmware hang** (kill heartbeat) → safety relay opens → pump de-powered, valve state
      irrelevant to dryness.
- [ ] **Max-runtime ceiling** → ATtiny trips on its own clock → pump off; GPIO36 + SPA show
      tripped.
- [ ] **Welded pump relay** (simulate: jumper GPIO22 high) → idle pump still dead, because
      the armed rail is open between runs.
- [ ] **Stuck-open valve** (force a FET on) → no flow with pump unpowered; flow sensor flags
      idle flow during a run.
- [ ] **Lost Wi-Fi** → scheduled run still fires from flash (pull the phone off the network
      mid-schedule).
- [ ] **Boot state** → every FET off, pump off, diverter at plain-water rest, alive LED
      blinking.

This is the firmware spec **§17 acceptance checklist** expressed as wiring tests — it is the
**definition of done** for the electrical build. Pass it on the bench before anything goes
to the tunnel; wet hardware (real water through the real pump) is the final gate (Phase 6),
not part of this electrical bring-up.

---

## Quick reference — the order in one breath

1. **24V rail** + fuse/TVS/reverse-protect + **master switch** → meter the bus.
2. **Bucks** (3.3V, 5V) with outputs floating → meter the outputs.
3. **Common ground** star point → continuity.
4. **ESP32 alone** → alive LED blinks, SPA loads.
5. **Watchdog + safety relay** → armed 24V live *only* during a healthy run; dies on every fault.
6. **One valve FET** (gate R + pulldown + TVS, raw 24V) → boots off, actuates on command.
7. **Remaining 4 valve FETs** → each off at boot, diverter rests plain-water.
8. **Pump relay on the armed rail** (LAST) → runs only with both keys; dies on heartbeat/master loss.
9. **Flow sensor** via 5V→3.3V level shift → pulses on GPIO27, ≤3.3V.
10. **Walk the §6/§17 fail-dry table** → that's done.

*Power one rail at a time. Meter before you proceed. Pump goes on last.*
