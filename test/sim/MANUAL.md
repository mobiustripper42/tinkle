# Interactive SPA sim session (#62) — manual walkthrough

The phone-driven end-to-end run, in the VS Code Wokwi simulator. This is the
SPA-driven half of the sim tier (DEC-019 made V1 phone-only, so the run path is driven
from the phone UI, not buttons). The headless `wokwi-cli` checks are separate — see
`README.md`.

## Prereqs

- The **Wokwi for VS Code** extension (free license) — the only runtime that honors
  `[[net.forward]]` and can compile/boot the local binary.
- `pio run -e esp32_sim` built (`wokwi.toml` points at `.pio/build/esp32_sim/firmware.bin`).
- **Headless VPS note (mill-dev):** there's no local browser, and the free
  `localhost:8180` forward drops the SPA after ~20 s (socket churn — see the next
  section). For a stable connection use the **Private Gateway** (recommended on
  mill-dev); the old 8180 forward still works for a quick look if you reload to renew.

## Stable SPA connection — the Private Wokwi Gateway (mill-dev)

The free path (VS Code forwarding `localhost:8180`) opens a **new network socket on every
SPA poll**; the simulated ESP32's small socket pool fills up and the SPA goes dead after
~16–25 s while the sim keeps running (LEDs keep blinking — it's not a crash). The fix is
the **Private Wokwi Gateway**: a small Wokwi program that runs on mill-dev and gives the
simulator a stable, keep-alive connection to the device's web server. It needs a **paid
Wokwi plan**, and the VS Code extension's license must be issued from that same paid
account (VS Code: `F1` → **"Wokwi: Request a New License"** while logged into the paid
account on wokwi.com — paying on one account but holding a license from another is the
classic trap).

**One-time setup (already done, for reference):** the gateway binary lives at
`~/wokwigw/wokwigw-linux` (Linux x86 build of `wokwi/wokwigw`), and `wokwi.toml` already
carries the `[net] gateway = "ws://localhost:9011"` line that tells the extension to use it.

**Turn it on — every session, BEFORE starting the sim:**

1. Open a terminal on mill-dev and run this — **leave the terminal open** (closing it
   stops the gateway; Ctrl+C also stops it):
   ```
   ~/wokwigw/wokwigw-linux --forward 9080:10.13.37.2:80
   ```
   You should see `Listening on TCP Port 9011` and `:9080 -> 10.13.37.2:80`.
2. In VS Code: `Ctrl+Shift+P` → **Restart Simulation** (or **Wokwi: Start Simulator** if
   it isn't running). The sim re-reads `wokwi.toml` and attaches — the gateway terminal
   prints `Client connected`.
3. Open the SPA at **`http://localhost:9080/`** — NOT 8180 (that's the old, dropping path).

**Did it work?** The SPA's status shows the device IP as **`10.13.37.2`** (the free public
gateway shows `10.10.0.2`). The SPA now stays up at the normal 1 Hz refresh, indefinitely.

**On your phone (Pixel, Chrome, Tailscale connected):** publish port 9080 onto Tailscale,
then open the URL:
```
tailscale serve --bg --https=8443 127.0.0.1:9080
```
→ `https://mill-dev.tail7e2bfd.ts.net:8443/`. Stop sharing later with
`tailscale serve --https=8443 off`. (Safari is unsupported by the gateway — use Chrome.)

## Bring-up

1. `pio run -e esp32_sim` → SUCCESS. (TINKLE_SIM shortens valve travel to 1 s, grace
   3 s, default run 10 s, so a run is watchable.)
2. VS Code: **F1 → "Wokwi: Start Simulator"**. The board boots; the **ALIVE** LED
   starts blinking ~1 Hz → firmware is ticking.
3. The firmware joins **Wokwi-GUEST** on its own (empty sim NVS → `TINKLE_SIM` default
   creds), then NTP syncs.
4. Open the SPA: **`http://localhost:9080/`** if you started the Private Gateway
   (recommended — stable; see the section above), or `http://localhost:8180/` for the
   free forward (drops after ~20 s, reload to renew). The SPA **Status/home** screen
   loads: state IDLE, clock shows `HH:MM` once NTP lands.

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
