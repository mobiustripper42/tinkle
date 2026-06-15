# Sim-tier e2e (DEC-004 tier 2)

Headless, automated end-to-end tests of the **real `esp32_sim` firmware** running in
the Wokwi cloud simulator, driven by `wokwi-cli` automation scenarios. Each scenario
presses buttons / flips the FLOW switch and asserts **actuator pin states**
(`expect-pin`) — the pump relay and zone valve FETs — so a pass means the firmware
actually commanded the right outputs, not just that it printed the right log line.

## Why pin assertions instead of serial / the SPA

The `#62` checklist originally called for watching the serial console
(`joining Wokwi-GUEST` → `WiFi STA up`) and opening the SPA at `localhost:8180`.
Neither is usable through `wokwi-cli` today:

- **Serial is dead.** `wokwi-cli 0.26.1` crashes parsing the current Simulation
  API's serial frames (`Invalid field value: 'bytes'`), so the stream is empty —
  the upstream "Blank Terminal Problem"
  ([wokwi/wokwi-features#1106](https://github.com/wokwi/wokwi-features/issues/1106)),
  which also affects the VS Code extension's integrated-terminal serial. The firmware
  runs fine (screenshots confirm the panel + run sequence); only the serial transport
  is broken.
- **No port forward.** `[[net.forward]]` (the `localhost:8180 → target:80` line in
  `wokwi.toml`) is a VS Code-extension feature; `wokwi-cli` runs the sim in Wokwi's
  cloud and never binds a local port. The SPA is therefore deferred to the bench tier.

`expect-pin` sidesteps both and is a *better* CI gate anyway — deterministic, no log
scraping. Screenshots of the TM1637 are captured alongside for human diagnostics
(run state / fault code) but are not the gate.

## Running

Pinned to **`wokwi-cli` 0.26.1** — `run.sh` tells a real failure from a transient one
by wokwi-cli's assertion wording (`... expected to be X but was Y`); a future version
that rephrases it could mask a real failure as a retryable transient. Check the version
(`wokwi-cli --version`) if assertions start behaving oddly after an upgrade.

```bash
pio run -e esp32_sim                              # build the binary wokwi.toml points at
export WOKWI_CLI_TOKEN=...                         # https://wokwi.com/dashboard/ci (free)
test/sim/run.sh                                    # all scenarios
test/sim/run.sh 02 06                              # only matching scenarios
```

Each scenario exits non-zero on the first failed assertion; `run.sh` aggregates.
Screenshots land in `test/sim/screenshots/` (gitignored).

**Cloud quota.** `wokwi-cli` runs the sim in Wokwi's cloud. The free CI tier has a
**monthly CI-minute quota** (once spent: `API Error: You have used up your Free plan
monthly CI minute quota` — `run.sh` detects this and aborts, since no retry helps
until it resets next month). It also runs the sim at a variable 3–4× slowdown. `run.sh` accommodates both: a generous
wall-clock `--timeout`, a gap between launches, and **retry of transient
(non-assertion) failures** — a real assertion failure is never retried. A single
suite pass (7 sims) is comfortably within quota; running it many times in a few
minutes will exhaust the free tier until it replenishes. Assertions are deterministic
regardless (scenario `delay` is simulation time), so a transient is always the cloud,
never the test.

## Coverage

| Scenario | Asserts |
|----------|---------|
| `01_zone1_noflow_e1`      | Button run with no flow → 3s grace → **fail dry** (pump off, valve closed) + E1 |
| `02_zone1_happy`          | Run with flow survives the grace, completes the ~10s run, **clean auto-stop** |
| `03_flowdrop_mid_run_e1`  | Flow lost mid-run → no-flow fault; verified by the **latched** refusal of a new run |
| `04_idle_unexpected_flow_e2` | Flow at idle (>50 pulses/5s) → E2; a subsequent run is **refused** |
| `05_fault_clear_longpress`| ≥3s long-press clears a latched fault → a fresh run **starts again** (DEC-006) |
| `06_multizone`            | B2→Zone 2 (GPIO14), B3→Zone 3 (GPIO16), shared pump; one button per zone (DEC-006) |
| `07_press_stops_run`      | Pressing a running zone's button **stops** it — manual override (DEC-006) |

## Timing notes (TINKLE_SIM constants)

The `esp32_sim` build shortens the physical timings so a run is watchable: valve
travel **1s**, flow grace **3s**, default run **10s**, idle-flow threshold **50
pulses / 5s** (~3.3s at the 15 Hz fake-flow source). The GPM rate is an 8-sample
1 Hz ring, so it decays to zero ~7s after flow stops — which is why `03` gates on the
fault **latch** rather than racing the pump-off instant against the 10s run's end.

## Not covered here

- **Reboot/NVS persistence** (schedule + calibration K surviving a power cycle): not
  expressible in a single `wokwi-cli` run; covered by the native tests (`pio test -e
  native`) and the bench tier.
- **The SPA / API** (schedule edits, calibration UI, live countdown in the browser):
  deferred to the bench tier per above.
- **Scheduled-start firing**: the scenarios drive runs via the buttons; NTP/schedule
  timing is left to native tests + bench.
