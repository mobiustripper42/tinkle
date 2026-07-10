# Tinkle — Electrical Build, Step by Step

Electrical only (plumbing's covered). Do the steps in order. Each step: **wire it, then
check it.** Don't move on until the check passes. If a check fails, stop.

Two rules: **one thing at a time** (never power everything at once), and **the pump goes
on last.**

We build in two parts:
- **Part 1 — on the bench, with LEDs.** USB powers the ESP32; LEDs stand in for valves and
  the pump. Nothing here can hurt you or the hardware. This proves the firmware and your
  wiring.
- **Part 2 — real power and real loads.** 24V, valves, the safety relay, the pump.

---

## The pin map

The ESP32 is the brain. Each job is one numbered pin on the board (silkscreen labels like
`D13`, `G13`, or just `13`).

| ESP32 pin | Drives | Type |
|---|---|---|
| 2  | "Alive" heartbeat LED | output (external LED — see note below) |
| 13 | Zone 1 valve | output |
| 14 | Zone 2 valve | output |
| 16 | Zone 3 valve (hose) | output |
| 17 | Diverter — clean leg | output |
| 18 | Diverter — fert leg | output |
| 22 | Pump relay | output |
| 4  | Heartbeat → watchdog (ATtiny) | output |
| 36 | "Tripped" ← watchdog (ATtiny) | input |
| 27 | Flow sensor pulse | input |

`GND` = ground = the common "minus." Every part's ground ties back to the **same** ground.
The ESP32 has several pins labeled `GND`; any of them works.

> **This board has no software-controllable onboard LED.** The bright LED that's always on
> the moment USB is plugged in is the **power** LED — hardwired, the firmware can't touch it.
> The ~1 Hz "alive" heartbeat needs an **external LED on pin 2**, wired exactly like the
> output LEDs (long leg → pin 2, short leg → 330Ω → GND). Wire it in Step 1, below.

---

## What a FET is (read once)

A **FET** is an electronic switch. The ESP32 can't push enough power to open a valve
directly, so each valve gets a FET: the ESP32 tells the FET "on," the FET lets the 24V
through to the valve.

The part is an **IRLZ44N** — a black plastic block with a metal tab and **three legs**.
Hold it with the printed label facing you, legs pointing down. Left to right:

```
   [ metal tab ]
   |   |   |
   G   D   S
 (gate)(drain)(source)
```

- **Gate (G, left leg)** — the control. Connects to an ESP32 pin (through a small resistor).
- **Drain (D, middle leg)** — the load. Connects to one wire of the valve.
- **Source (S, right leg)** — connects to **GND**.

Two resistors per FET:
- **100Ω** between the ESP32 pin and the Gate (protects the pin).
- **100k** between the Gate and GND (holds the valve **off** at power-on, before the firmware
  takes over).

When the ESP32 pin goes HIGH, the FET connects Drain to Source → 24V flows through the valve
→ valve opens. Pin LOW → valve closes. That's the whole trick, repeated per valve.

---

## Flashing the firmware

This puts the Tinkle software onto the ESP32. **Needs a computer with a USB cable — you
can't do it from a phone.** Do it once now; after that, USB also powers the ESP32 for all of
Part 1.

1. On the computer, install PlatformIO: `pip install platformio` (or the PlatformIO VS Code
   extension).
2. Plug the ESP32 into the computer with a **data** USB cable (some cheap cables are
   charge-only — those won't work).
3. In the Tinkle repo folder, run:
   ```
   pio run -e esp32 -t upload
   ```
4. Watch the serial output:
   ```
   pio device monitor -b 115200
   ```

**Check:** the upload completes and the serial monitor prints the boot/heartbeat log — that's
the firmware running. (There's no onboard alive LED to watch yet; you wire one in Step 1. The
steady LED already lit is just the power LED.) If the upload can't find the board, it's
usually the USB cable or a missing USB driver (most DevKitC boards are CP2102 or CH340 —
install that driver for your OS).

---

# Part 1 — Bench test with LEDs

LED legs: the **long** leg is `+` (anode), the **short** leg is `−` (cathode).

## Step 1 — ESP32 alive (and one pull-up that keeps the bench quiet)
Two small bits of wiring before any output test:

- **Alive LED on pin 2.** ESP32 **pin 2** → LED long leg; LED short leg → **330Ω** → **GND**
  (same pattern as every output LED below). This is the heartbeat indicator — there's no
  onboard one on this board.
- **Pull-up on pin 36.** A **10kΩ resistor from pin 36 to 3.3V**, plus a **100nF cap from
  pin 36 to GND** (noise filter). Pin 36 is the watchdog "tripped" input; nothing drives it
  yet in Part 1, so it floats and the firmware reads a false trip — you'll hit fault
  **E3 (watchdog)** the moment you start a run. The pull-up holds it in the safe state until
  the real ATtiny lands in Step 8. (Step 8 expects this same pull-up + cap — leave them.)

**Check:** USB plugged in, the **pin-2 LED blinks ~once a second.** That's the firmware
running. No E3 on the phone when you start a run in Step 2.

## Step 2 — one output drives an LED
Pick Zone 1 (pin 13). On a breadboard:
- ESP32 **pin 13** → LED long leg.
- LED short leg → **330Ω resistor** → ESP32 **GND**.

**Check:** on your phone, join the ESP32's Wi-Fi (`Tinkle-Setup`, or your network), open
`http://tinkle.local`, and start a manual run on Zone 1. The LED lights. Stop it — the LED
goes off.

## Step 3 — the rest of the outputs
Repeat Step 2 for each, same wiring (pin → LED long leg → 330Ω → GND):

| LED for | ESP32 pin |
|---|---|
| Zone 2 | 14 |
| Zone 3 | 16 |
| Diverter clean | 17 |
| Diverter fert | 18 |
| Pump | 22 |

**Check:** each LED lights only when you run that thing from the phone. Starting Zone 1
must not light Zone 2's LED.

> Done with Part 1 you've proven the firmware and know every pin works. Now the real power.

---

# Part 2 — Real power and loads

## Step 4 — the 24V rail
Wire the power supply and its protection. **Leave everything else unplugged.**
- AC mains → Mean Well **LRS-150-24** input (through an AC switch if you have one).
- LRS `V+` → **10A fuse** → your `24V+` rail.
- LRS `V−` → your `GND` rail.
- **TVS across the rail:** a **1.5KE30CA** transient-suppressor diode from `24V+` to `GND` —
  brownout/transient insurance for everything downstream. The CA part is bidirectional: no
  band, either orientation works. It clamps spikes only; in normal operation it does nothing,
  so it won't affect the meter reading below.

**Check:** meter across the rails reads **~24V**. Switch off → 0V. Confirm `+` and `−` are
where you think they are; mark them.

## Step 5 — the 5V buck
The buck drops 24V to 5V (for the flow sensor and, in final assembly, the ESP32).
- `24V+` → buck **IN+**, `GND` → buck **IN−**.
- Leave buck **OUT** unconnected for now.

**Check:** meter the buck **OUT+ to OUT−** = **~5V**. If it's adjustable and reads anything
higher, turn it down to 5V now, before anything is connected to it.

## Step 6 — one valve on a FET
Build a single Zone 1 channel (pin 13). Wire the FET as in "What a FET is":
- ESP32 **pin 13** → **100Ω** → FET **Gate**.
- FET **Gate** → **100k** → **GND**.
- FET **Source** → **GND**.
- FET **Drain** → one valve wire.
- Other valve wire → **24V+**.
- **1.5KE30CA TVS** across FET **Drain and Source** (bidirectional — either orientation).

Keep the ESP32 on USB for now (its GND and the 24V GND must be the **same** ground — tie
the rails together).

**Check:** at power-on the valve is **closed** (FET off — that's the 100k doing its job).
Run Zone 1 from the phone → the valve motors open (~6–10s). Stop → it returns closed.

## Step 7 — the rest of the valves
Repeat Step 6 for each, same FET wiring:

| Valve | ESP32 pin |
|---|---|
| Zone 2 | 14 |
| Zone 3 | 16 |
| Diverter clean | 17 |
| Diverter fert | 18 |

**Check:** each opens only on its own command. At rest: zones closed, clean leg open, fert
leg closed.

## Step 8 — the watchdog + safety relay
This is the safety: it cuts the pump's power if the firmware ever hangs. Build it **before**
the pump.

### Step 8.0 — flash the ATtiny first (one-time)
The watchdog is a **second chip** (ATtiny85) with its own firmware — it ships blank, so you
must flash it before it does anything. Unlike the ESP32, the ATtiny has **no USB port**, so
you flash it through an **Arduino Uno acting as a programmer**. You only do this once per chip.

You need: an Arduino Uno, six jumper wires, and a **10 µF electrolytic capacitor**.

1. **Turn the Uno into a programmer.** Load the stock **ArduinoISP** sketch onto it (Arduino
   IDE: *File → Examples → 11.ArduinoISP → ArduinoISP → Upload*). Headless alternative: build
   that sketch in a throwaway PlatformIO project (`board = uno`) and `pio run -t upload` it.
2. **Wire the Uno to the ATtiny** on a breadboard. ATtiny pin 1 is by the dot/notch; pins
   1–4 run down one side, 5–8 up the other:

   | Uno pin | → ATtiny pin |
   |---|---|
   | 13 | 7 |
   | 12 | 6 |
   | 11 | 5 |
   | 10 | 1 (reset) |
   | 5V | 8 (Vcc) |
   | GND | 4 (GND) |

   Then add the **10 µF cap across the Uno's RESET and GND** (short leg to GND). It stops the
   Uno resetting itself when the flasher connects — without it you get "not in sync" errors.
3. **Read the fuses first, then burn both fuses and flash** with avrdude through the Uno (the
   Uno powers the ATtiny while you do). Replace `/dev/ttyACM0` with the Uno's port if different:
   ```
   AVRD=~/.platformio/packages/tool-avrdude/avrdude
   CONF=~/.platformio/packages/tool-avrdude/avrdude.conf
   pio run -e attiny85                                                          # builds the .hex
   # Read first — know what you're starting from (factory: lfuse 0x62, hfuse 0xDF):
   $AVRD -C $CONF -c stk500v1 -P /dev/ttyACM0 -b 19200 -p attiny85 \
         -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h
   $AVRD -C $CONF -c stk500v1 -P /dev/ttyACM0 -b 19200 -p attiny85 -U lfuse:w:0xE2:m
   $AVRD -C $CONF -c stk500v1 -P /dev/ttyACM0 -b 19200 -p attiny85 -U hfuse:w:0xDD:m
   $AVRD -C $CONF -c stk500v1 -P /dev/ttyACM0 -b 19200 -p attiny85 \
         -U flash:w:.pio/build/attiny85/firmware.hex:i
   ```
   Both fuse steps matter:
   - **`lfuse 0xE2` (8 MHz):** the firmware's 2-second safety timeout assumes 8 MHz. Left at
     the factory 1 MHz, every timer runs 8× slow.
   - **`hfuse 0xDD` (brown-out detection at 2.7 V):** the factory `0xDF` leaves BOD **off**,
     and a quick repower lets the rail sag through the corruption zone without a clean
     reset — the chip wakes wedged with the trip line held (diagnosed Sessions 18–19). BOD
     forces a clean reset instead.

   (Use avrdude directly — PlatformIO's `-t fuses`/`-t upload` targets don't pass the port to
   a stk500v1 programmer.)

   **As-built record (2026-07-07):** all three project ATtiny85s carry `lfuse 0xE2` /
   `hfuse 0xDD` / `efuse 0xFF`, read back and verified after burning — the chips are
   interchangeable; it does not matter which one is in the socket. Any NEW chip must get the
   full sequence above before it joins the spares pool.

**Check:** the upload ends with `bytes of flash verified`. Then unplug the Uno — the ATtiny
keeps its program. Now wire it into the circuit:

- ESP32 **pin 4** → ATtiny **pin 7** (heartbeat input). Add a **10k pull-down from ATtiny
  pin 7 to GND** — this is a safety requirement, not optional: if the heartbeat wire ever
  falls off, the pull-down holds pin 7 low ("no heartbeat") so the watchdog disarms and the
  pump dies. Without it a broken heartbeat floats and can read as *alive*, leaving the pump
  armed — a fail-dangerous hole. The ESP32 still drives the line fine during runs.
- ATtiny **pin 5** ("tripped") → ESP32 **pin 36**, with a **10k resistor from pin 36 to 3.3V**
  and a **100nF cap from pin 36 to GND** (you already added this pull-up + cap in Step 1 —
  leave them). (You can equally reference these to the ATtiny end: `pin 5 → 10k → pin 8`,
  `pin 5 → 100nF → GND` — pin 5 and pin 36 are the same net. Keep the ATtiny on **3.3V** if
  you tie the pull-up to pin 8.)
- Also add a **10k from ATtiny pin 1 (RESET) to pin 8** — noise immunity so relay/pump
  switching can't glitch-reset the watchdog mid-run. (Internal pull-up usually covers it;
  cheap insurance next to the relays.)
- ATtiny **pin 6** (arm) → **safety relay module** `IN`; module `VCC`/`GND` → 5V buck and GND.
  Add a **10k from that `IN` to GND**: during ATtiny reset pin 6 goes Hi-Z, and the pulldown
  holds the relay **off** (fail-dry). Set the module's **H/L jumper to HIGH** (active-high) so
  boot-LOW = relay off.
- Safety relay contacts: `24V+` → relay `COM`; relay `NO` → a new rail call it **`24V-armed`**.
  (Pump power will come from `24V-armed`, not raw 24V.)

**Check:** with no run going, meter `24V-armed` = **0V** (relay open). Start a run → it reads
~24V (armed). Stop the run, or unplug pin 4 → it drops to 0V within ~2 seconds.

## Step 9 — the pump (last)
- ESP32 **pin 22** → pump relay module `IN`; its `VCC`/`GND` to 5V buck and GND. (H/L jumper
  to **HIGH**, same as Step 8; a 10k from `IN` to GND is good insurance here too.)
- Pump relay contacts: **`24V-armed`** → relay `COM`; relay `NO` → pump `+`. Pump `−` → GND.

On the bench, use an LED or the spare (cracked) pump as a stand-in — **don't run water.**

**Check (the important one):** the pump only powers when **both** a run is going (pin 22 on)
**and** the safety relay is armed (Step 8). Pull pin 4 mid-run → pump dies. Switch off AC →
pump dies.

## Step 10 — the flow sensor
- Sensor red → 5V buck OUT+, black → GND.
- Sensor yellow (pulse) → **level shifter** → ESP32 **pin 27**. (The pulse swings to 5V; the
  shifter drops it to 3.3V so it doesn't damage pin 27.)

**Check:** spin water/air through it (or tap the pulse line) → the flow reading on the phone
changes.

---

## Final check — fail-dry

Everything wired. Confirm each of these makes the pump dead:
- Switch off AC → pump dead.
- Start a run, then pull the heartbeat (pin 4) → pump dead within ~2s.
- At power-on, before you do anything → all valves closed, pump off, pin-2 alive LED blinking.

That's the build. Wet test (real water) comes later.

---

## Connection list (every wire)

The whole netlist in one place, consistent with the steps above. Build **power rails** first,
then the **protoboard**, then the **field** wires. One common ground everywhere (`GND`).

### Power rails
| From | To |
|---|---|
| AC hot / neutral / ground | LRS-150-24 **L / N / ⏚** |
| LRS **V+** | 10A fuse → **24V+** rail |
| LRS **V−** | **GND** rail |
| 24V+ | buck **IN+** |
| GND | buck **IN−** |
| buck **OUT+** | **5V** rail |
| buck **OUT−** | GND rail |

### ESP32 ↔ protoboard (the jumpers between the two boards)
| ESP32 pin | Goes to |
|---|---|
| 5V / VIN | 5V rail |
| GND | GND rail |
| 3V3 | 3.3V node (feeds the pin-36 pull-up + level-shifter LV) |
| 13 | Zone 1 FET gate (via 100Ω) |
| 14 | Zone 2 FET gate (via 100Ω) |
| 16 | Zone 3 FET gate (via 100Ω) |
| 17 | Diverter-clean FET gate (via 100Ω) |
| 18 | Diverter-fert FET gate (via 100Ω) |
| 22 | Pump relay module **IN** |
| 4 | ATtiny **pin 7** (heartbeat) |
| 36 | ATtiny **pin 5** (trip) **+ 10k to 3V3 + 100nF to GND** |
| 27 | Level-shifter LV-out |
| 2 | onboard LED — **no wire** |

### On the protoboard
**ATtiny85** (DIP pins; pin 1 by the notch):
| ATtiny pin | Goes to |
|---|---|
| 8 (VCC) | 5V rail |
| 4 (GND) | GND rail |
| 7 (heartbeat in) | ESP32 pin 4, **+ 10k pin 7 → GND** (broken-wire = "no heartbeat" = fail-dry) |
| 6 (arm out) | Safety relay **IN**, **+ 10k IN → GND** (holds off during ATtiny reset) |
| 5 (trip out) | ESP32 pin 36 |
| 1 (RESET) | **10k → pin 8** (noise immunity) |

**Each valve FET — IRLZ44N ×5** (Z1=13, Z2=14, Z3=16, DIV-clean=17, DIV-fert=18):
| FET leg | Goes to |
|---|---|
| Gate | its ESP32 pin via **100Ω**, **+ 100k gate → GND** |
| Source | GND rail |
| Drain | that valve's return wire (field) |
| Drain↔Source | **1.5KE30CA TVS** across the two |

**Relay modules ×2** (H/L jumper → HIGH):
| Module pin | Goes to |
|---|---|
| VCC (both) | 5V rail |
| GND (both) | GND rail |
| Pump IN | ESP32 pin 22 |
| Safety IN | ATtiny pin 6 (**+ 10k IN → GND**) |

**Level shifter** (flow pulse 5V → 3.3V):
| Pin | Goes to |
|---|---|
| HV | 5V rail |
| LV | 3V3 |
| GND | GND rail |
| HV channel in | flow sensor pulse (field) |
| LV channel out | ESP32 pin 27 |

### External — box to field
| Device | Wiring |
|---|---|
| Zone 1 valve | wire A → **24V+**; wire B → Z1 FET drain |
| Zone 2 valve | A → 24V+; B → Z2 FET drain |
| Zone 3 valve | A → 24V+; B → Z3 FET drain |
| Diverter clean valve | A → 24V+; B → DIV-clean FET drain |
| Diverter fert valve | A → 24V+; B → DIV-fert FET drain |
| **Pump** | 24V+ → safety **COM**; safety **NO** → pump **COM**; pump **NO** → pump **+**; pump **−** → GND |
| Flow sensor (Leridian) | red → 5V; black → GND; yellow → level-shifter HV-in |
| AC mains | → LRS **L / N / ⏚** |

**Two rules baked in:** valves run on **raw 24V+** (a stuck valve passes nothing with the pump
off); only the **pump** goes through the two relays in series (armed), so it has power only
when both close — the fail-dry gate.

> Possible simplification (verify, don't assume): the Leridian is rated to run at **3.3V**.
> Powered at 3.3V with the ESP32's internal pull-up on pin 27, its pulse would already be
> 3.3V and the **level shifter drops out**. Confirm on the bench before removing it.
