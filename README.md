# Tinkle

Automated drip-irrigation controller for Bay Branch Farm's high tunnels —
scheduled, per-zone watering with reliable manual override. An embedded project:
**ESP32 firmware** + an **ATtiny85 hardware watchdog** + a **vanilla-JS phone UI**
served from flash. Guiding philosophy: **fail dry**, **local autonomy**,
**build-for-three / populate-one**.

For the why and the what, start with [`docs/SPEC.md`](docs/SPEC.md) (scope) and
[`CLAUDE.md`](CLAUDE.md) (architecture). Hardware truth lives in
[`docs/tinkle_v1_spec.md`](docs/tinkle_v1_spec.md); behavior in
[`docs/tinkle_firmware_spec.md`](docs/tinkle_firmware_spec.md); pins in
[`docs/tinkle_wiring.html`](docs/tinkle_wiring.html).

## Repo layout

```
src/esp32    ESP32 firmware (Arduino-ESP32)
src/attiny   ATtiny85 safety watchdog (separate binary)
src/core     platform-independent logic — compiles for BOTH the ESP32 and the host
test/        native unit tests (Unity), the load-bearing test tier
web/         vanilla HTML/CSS/JS SPA (gzipped into PROGMEM)
docs/        SPEC, DECISIONS, specs, wiring, project plan
platformio.ini   build envs: esp32 / attiny85 / native
wokwi.toml       simulation config (tier 2)
diagram.json     Wokwi breadboard layout
```

## Bringing a new machine up to speed

Tested on Ubuntu 24.04. macOS is the same except where noted.

### 1. Host C++ toolchain (needed for native tests)

```bash
sudo apt install -y build-essential        # g++ + make
```

The native test tier (`pio test -e native`) compiles `src/core` on the host — its
only real prerequisite is a host C++ compiler. On macOS: `xcode-select --install`.

### 2. PlatformIO Core (the `pio` CLI)

> **Do NOT `apt install platformio`.** The Debian/Ubuntu package is stuck at 4.3.4
> (2020) and crashes on import against modern Click —
> `AttributeError: 'PlatformioCLI' object has no attribute 'resultcallback'` (Click
> renamed it `result_callback` in 8.1). If it's already installed, remove it first:
> `sudo apt remove -y platformio`.

Ubuntu 24.04 ships no `pip` and blocks system-wide pip installs (PEP 668), so use
PlatformIO's official installer — it builds its own isolated virtualenv under
`~/.platformio/penv` with its own pinned dependencies (which is exactly why it
dodges the system-Click breakage above):

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py
```

Then put `pio` on your PATH (add to `~/.bashrc` / `~/.zshrc` to make it stick):

```bash
export PATH="$PATH:$HOME/.platformio/penv/bin"
pio --version        # confirm
```

(If you use the VS Code **PlatformIO IDE** extension instead, it bundles its own
Core and you can skip this — but the CLI is what the commands below assume.)

### 3. Build + test

```bash
pio test -e native           # host unit tests — fast, no hardware, run these first
pio run -e esp32             # build the firmware (FIRST run downloads the
                             # espressif32 platform + toolchain + libs — large + slow)
pio run -e esp32_sim         # same firmware, but -D TINKLE_SIM (short valve travel +
                             # run duration) — this is what the simulator runs
pio run -e attiny85          # build the watchdog binary
```

There are two ESP32 builds: **`esp32`** is the real-hardware firmware (§15 timings —
6-10 s valve travel, 10 min default run); **`esp32_sim`** adds `-D TINKLE_SIM`, which
shortens those to seconds so a run is watchable in Wokwi. Build/flash `esp32` for
hardware; build `esp32_sim` for the simulator. `wokwi.toml` points at
`.pio/build/esp32_sim/firmware.{bin,elf}`, so build that env before starting the sim.

## Running the simulation (Wokwi)

Wokwi boots the **real ESP32 firmware** in a virtual breadboard (DEC-004 tier 2):
the TM1637 panel, the three zone buttons, and status LEDs for the pump, the zone
valve FETs, and the diverter legs — enough to watch a full run sequence without
hardware.

**Fake flow:** the sim build generates its own hall pulses (15 Hz LEDC square
wave on GPIO19, ≈2 GPM at the seed K), looped back to the flow input through the
diagram's **"FLOW on/off" slide switch** — stock Wokwi parts, no custom chips.
The switch **boots quiet** (flow at boot would latch the idle-flow fault
unprovoked). Slide it up as a run starts; down mid-run → E1 no-flow; up while
idle → E2 unexpected flow.

The sim also brings up the **whole web stack** (#62): with empty sim NVS the
firmware joins `Wokwi-GUEST` automatically, NTP syncs the clock (so schedules
fire), and `wokwi.toml` forwards the SPA/API to the host:
**`http://localhost:8180/`** while the sim runs (on a remote VPS, let VS Code
forward port 8180, or tunnel it). No `?mock=1` here — this is the real API.

**Always `pio run -e esp32_sim` first** (the sim runs the built binary, it does not
compile — and `esp32_sim` is the one with the watchable short timings).

### Option A — VS Code (primary)

1. Install the **Wokwi for VS Code** extension.
2. Activate the free license once: `F1` → **"Wokwi: Request a New License"** (opens
   a browser; one-time).
3. `F1` → **"Wokwi: Start Simulator"**. It reads `wokwi.toml` + `diagram.json`.

### Option B — Wokwi CLI (headless / no VS Code)

```bash
curl -L https://wokwi.com/ci/install.sh | sh         # installs wokwi-cli
export WOKWI_CLI_TOKEN=...   # from https://wokwi.com/dashboard/ci (free)
wokwi-cli .                  # runs wokwi.toml + diagram.json in this dir
```

## Flashing real hardware (Phase 6)

```bash
pio run -e esp32 -t upload          # flash the ESP32 over USB
pio device monitor -b 115200        # serial monitor
```

The ATtiny85 needs an ISP programmer; set `upload_protocol` in `platformio.ini`
(`usbasp` / `arduino-as-isp` / `micronucleus`) on the bench.

## Notes

- **Session workflow.** This project uses session skills (`/its-alive`,
  `/kill-this`, etc.). Per-session files live on an orphan `sessions` branch checked
  out at `.sessions-worktree/` — created automatically by `/its-alive` on first run;
  you don't set it up by hand.
- **Why `native` pins `-std=gnu++11`.** The host tier compiles `src/core` under the
  same standard the ESP32 uses (arduino-esp32 2.0.x), so code that's illegal
  on-target can't sneak through host tests. Don't bump it independently.
