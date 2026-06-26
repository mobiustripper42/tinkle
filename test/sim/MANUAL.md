# Interactive SPA sim session (#62) — manual walkthrough

The phone-driven end-to-end run, in the VS Code Wokwi simulator. This is the
SPA-driven half of the sim tier (DEC-019 made V1 phone-only, so the run path is driven
from the phone UI, not buttons). The headless `wokwi-cli` checks are separate — see
`README.md`.

## Prereqs

- The **Wokwi for VS Code** extension (free license) — the only runtime that honors
  `[[net.forward]]` and can compile/boot the local binary.
- `pio run -e esp32_sim` built (`wokwi.toml` points at `.pio/build/esp32_sim/firmware.bin`).
- **Headless VPS (mill-dev):** no local browser — drive the SPA from the phone via the
  Private Gateway (next section).

## Stable SPA connection — Private Wokwi Gateway (mill-dev → phone)

The free `net.forward` 8180 path drops the SPA every ~20 s (socket churn). The Private
Gateway fixes it. One-time: paid Wokwi plan + VS Code license on that account; binary at
`~/wokwigw/wokwigw-linux`; `wokwi.toml` has `[net] gateway = "ws://localhost:9011"`.

Every session, in order:

1. `~/wokwigw/wokwigw-linux --forward 9080:10.13.37.2:80`  — leave it running
2. VS Code: `Ctrl+Shift+P` → **Start/Restart Simulation**  — WiFi connects
3. `tailscale serve --bg --https=8443 127.0.0.1:9080`

Open **`https://mill-dev.tail7e2bfd.ts.net:8443/`** on the phone (Tailscale on, Chrome).

## Bring-up

1. `pio run -e esp32_sim` → SUCCESS. (TINKLE_SIM shortens valve travel to 1 s, grace
   3 s, default run 10 s, so a run is watchable.)
2. VS Code: **F1 → "Wokwi: Start Simulator"**. The board boots; the **ALIVE** LED
   starts blinking ~1 Hz → firmware is ticking.
3. The firmware joins **Wokwi-GUEST** on its own (empty sim NVS → `TINKLE_SIM` default
   creds), then NTP syncs.
4. Open the SPA on the phone at `https://mill-dev.tail7e2bfd.ts.net:8443/` (Private
   Gateway — see above). The SPA **Status/home** screen loads: state IDLE, clock shows
   `HH:MM` once NTP lands.

   *Expected:* in the diagram, every status LED is off except ALIVE (blinking): PUMP
   off, VALVE Z1/Z2/Z3 off, DIV CLEAN/DIV FERT off. FLOW switch down (quiet).

## 1 — A clean run (happy path)

1. SPA → **Manual run** → pick **Zone 1**, leave the default duration, fertigate off →
   **START**.
   *Expected:* DIV stays plain; **VALVE Z1** LED on (~1 s travel); then **PUMP** LED on;
   Status/home shows RUNNING with a live MM:SS countdown.
2. Within the first ~2 s, slide **FLOW on**.
   *Expected:* the run survives the 3 s grace (flow is present); Status shows live GPM
   (~2.0). The run holds for the ~10 s duration.
3. Let it finish (or hit **STOP ALL**).
   *Expected:* **PUMP** LED off first (source cut), then **VALVE Z1** off (~1 s
   cap-return); back to IDLE; the last-run summary shows the gallons.
4. Slide **FLOW off**.

## 2 — No-flow fault (E1) during a run

1. With FLOW **off** (down), start a **Zone 1** run as above.
   *Expected:* VALVE Z1 on, PUMP on, countdown running.
2. Leave FLOW off through the 3 s grace.
   *Expected:* past grace, the firmware faults **E1 / no-flow**: **PUMP** off, **VALVE
   Z1** closes, state → FAULT. Status/home shows the latched fault prominently; the
   ALIVE LED keeps blinking (it is not a fault indicator).
3. SPA → **Faults** → clear (only works once the condition is resolved — there's no
   flow to resolve here, so the clear takes since the run already ended).

## 3 — Unexpected idle flow (E2)

1. From IDLE, with no run, slide **FLOW on**.
   *Expected:* after ~3.3 s (50 pulses / 5 s window) the firmware latches **E2 /
   unexpected flow**, commands safe state (it was already idle — nothing to shut, which
   is the point: nothing energizes). Status/home shows the persistent **⚠ FLOW** fault.
2. Slide **FLOW off** (resolves the condition), then SPA → **Faults** → **clear**.
   *Expected:* with flow gone, the resolved-condition gate lets the clear through →
   back to IDLE.

   *This is the one the headless `02_idle_unexpected_flow` scenario also exercises — but
   headless can only assert "nothing energized," not the latch. Here you see the banner.*

## 4 — Fertigation + calibration (optional)

- **Manual run** with **fertigate on** → DIV FERT LED on, DIV CLEAN off during the run
  (the diverter travels only on change); reverts at SETTLE.
- **Calibration** screen → guided run into a known volume → K saved to NVS; survives a
  sim reboot (stop/start the simulator).

## Limitations (state them honestly)

- **No serial in `wokwi-cli`**, and this session is **VS Code-only** (the CLI can't
  forward 8180). So this walkthrough is **manual**, not CI-automated — the automated
  slice is the two headless `expect-pin` scenarios.
- The diagram omits LED series resistors (sim convenience); the breadboard bench (tier
  3) uses real current-limiting per the wiring doc.
- Cloud sim runs ~3–4× slower than wall-clock; the TINKLE_SIM short timings keep a run
  watchable regardless.
