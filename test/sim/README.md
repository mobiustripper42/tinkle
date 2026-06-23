# Sim-tier e2e (DEC-004 tier 2)

Runs the **real `esp32_sim` firmware** in the Wokwi simulator. Phone-only (DEC-019)
split this tier in two:

| Mode | Driver | What it covers | Automatable? |
|------|--------|----------------|--------------|
| **A. Interactive** (VS Code Wokwi) | the **SPA**, over `net.forward` 8180→80 | the full run path — start/stop a run, fertigate, calibrate, watch the live countdown + valve/pump LEDs; provoke E1/E2 with the FLOW switch | no (manual — see `MANUAL.md`) |
| **B. Headless** (`wokwi-cli` + `run.sh`) | the **FLOW switch** + `expect-pin` | the fail-dry checks that need **no run**: boot-safe-state and idle-unexpected-flow | yes |

**Why the split.** The old (button-driven) scenarios triggered runs by pressing
zone buttons. DEC-019 cut the buttons — V1 is phone-only. The headless `wokwi-cli`
runner **can't reach the SPA** (`[[net.forward]]` is a VS Code-extension feature the
CLI doesn't implement) and **can't read serial** (`wokwi-cli` crashes parsing the
current Simulation API's serial frames — the upstream "Blank Terminal Problem",
[wokwi/wokwi-features#1106](https://github.com/wokwi/wokwi-features/issues/1106)). So
there is no headless way to *start* a run anymore. The run-dependent coverage moved to
the interactive SPA session (A); the headless tier (B) keeps the two fail-dry checks
that need no run, asserted via `expect-pin` on the actuator GPIOs.

## A — Interactive SPA session (primary, #62)

The end-to-end "schedule/manual start → run → stop" walkthrough lives in
**`MANUAL.md`**. In short: `pio run -e esp32_sim`, start the Wokwi sim in VS Code, let
VS Code forward `localhost:8180`, open the SPA, and drive a run from the phone UI while
watching the valve + pump LEDs and the FLOW switch.

## B — Headless checks

```bash
pio run -e esp32_sim                 # build the binary wokwi.toml points at
export WOKWI_CLI_TOKEN=...            # https://wokwi.com/dashboard/ci (free)
test/sim/run.sh                      # both scenarios
test/sim/run.sh 02                   # only matching
```

Pinned to **`wokwi-cli` 0.26.1** — `run.sh` tells a real assertion failure from a
transient cloud failure by wokwi-cli's wording (`... expected to be X but was Y`); a
future version that rephrases it could mask a real failure. Check `wokwi-cli --version`
if assertions behave oddly after an upgrade.

| Scenario | Asserts |
|----------|---------|
| `01_boot_safe_state`       | At boot, every actuator rests dry: pump off (22), zones closed (13/14/16), diverter plain (clean 17 low=open, fert 18 low=closed). |
| `02_idle_unexpected_flow`  | FLOW on while idle → **fail dry**: the pump and all valves stay de-energized (the controller never chases unexpected flow). |

**What headless B can't see.** With no display and no serial, idle and a *latched*
`FAULT_UNEXPECTED_FLOW` both rest all-actuators-LOW — so `02` asserts the fail-dry
*property* (nothing energizes), not the latch itself. The latch (the SPA "⚠ FLOW"
banner + `/api/status` fault) is confirmed in session A. E1 (no-flow **during a run**),
the run sequence, and manual stop all need a run, so they're session-A only too.

**Cloud quota.** `wokwi-cli` runs the sim in Wokwi's cloud; the free CI tier has a
monthly CI-minute quota (`run.sh` detects exhaustion and aborts — no retry helps until
it resets) and runs the sim at a variable 3–4× slowdown. `run.sh` accommodates both: a
generous wall-clock `--timeout`, a gap between launches, and retry of transient
(non-assertion) failures. Assertions are deterministic (scenario `delay` is simulation
time), so a transient is always the cloud, never the test.

## Timing notes (TINKLE_SIM constants)

`esp32_sim` shortens the physical timings so a run is watchable: valve travel **1 s**,
flow grace **3 s**, default run **10 s**, idle-flow threshold **50 pulses / 5 s**
(~3.3 s at the 15 Hz fake-flow source).

## Fake flow source

The sim firmware emits its own 15 Hz hall pulses on **GPIO19** (LEDC, `TINKLE_SIM`
only), looped to the real flow input **GPIO27** through the **FLOW on/off** slide
switch in `diagram.json`. The switch **boots quiet** (common to GND) — a flow-on boot
would latch `FAULT_UNEXPECTED_FLOW` in ~3.3 s before you could do anything.

## Not covered here

- **The latch/fault surface** and anything needing a run (E1, run sequence, manual
  stop, fertigation, calibration): interactive session A + the bench tier.
- **Reboot/NVS persistence** (schedule + K surviving a power cycle): native tests
  (`pio test -e native`) + bench.
