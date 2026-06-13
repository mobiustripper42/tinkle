# Manual sim e2e (drive it by hand in VS Code)

For running the sim **interactively** in the Wokwi VS Code extension (local, no CLI,
no cloud quota) and watching the LEDs + TM1637. Serial is blank (upstream bug) — you
verify by the **panel**, not the log. Build first: `pio run -e esp32_sim`, then
F1 → **Wokwi: Start Simulator**. Keep the simulator tab **focused** or the sim pauses.

Sim timings are shortened: valve travel ~1s, flow grace ~3s, default run ~10s,
idle-flow trips after ~50 pulses / 5s (~3–4s of FLOW-up).

**Panel legend** — LEDs: `PUMP`, `VALVE Z1/Z2/Z3`, `DIV CLEAN/FERT`, `RING1/2/3`.
**Fault codes** (TM1637, "E" + digit flashing 1 Hz): `E1` no-flow · `E2` unexpected
flow · `E3` watchdog · `E4` cal range · `E5` clock.

**The clear rule:** a long-press (≥3s on any zone button) clears a latched fault **only
once its condition is resolved**. Long-pressing while the hazard still holds (e.g. FLOW
still up on an E2) is a deliberate no-op — resolve first, then clear.

| # | Do | Expect |
|---|----|--------|
| 1 | Start simulator | TM1637 shows a clock (ticking colon); all valve/pump LEDs off |
| **Happy run** | | |
| 2 | Click **B1** | `VALVE Z1` on → ~1s later `PUMP` on; TM1637 → MM:SS countdown |
| 3 | Slide **FLOW up** as it starts | run continues (no fault); LEDs hold through the run |
| 4 | Wait out the ~10s run | auto-stop: `PUMP` off, `VALVE Z1` off, TM1637 → clock |
| 5 | Slide **FLOW down** | (back to idle baseline) |
| **Manual stop** | | |
| 6 | Click **B1**, FLOW up, then click **B1** again | second press stops it: `PUMP` + `VALVE Z1` off immediately |
| 7 | FLOW down | idle |
| **E1 — no flow** | | |
| 8 | Click **B1**, leave **FLOW down** | `VALVE Z1` + `PUMP` on, then after ~3s grace: both off, TM1637 → **`E1`** |
| 9 | Long-press **B1** (≥3s) | `E1` clears → clock (condition already resolved — flow was never on) |
| **E1 — flow lost mid-run** | | |
| 10 | Click **B1**, FLOW **up** | running with flow (no fault past grace) |
| 11 | Mid-run, slide **FLOW down** | within a few seconds: `PUMP` + valve off, TM1637 → **`E1`** |
| 12 | Long-press **B1** | clears → clock |
| **E2 — unexpected flow + the clear gate** | | |
| 13 | At idle, slide **FLOW up**; wait ~5s | TM1637 → **`E2`** (flow seen while idle) |
| 14 | Long-press **B1** with **FLOW still up** | **no-op** — `E2` stays (clear refused, hazard active) |
| 15 | Slide **FLOW down**, wait ~5s, long-press **B1** | now `E2` clears → clock |
| 16 | With `E2` latched (before step 15), click **B1** | run **refused** — no `PUMP`, no valve (faults block runs) |
| **Per-zone** | | |
| 17 | Click **B2** (FLOW up) | `VALVE Z2` + `PUMP` on; stop with **B2** again |
| 18 | Click **B3** (FLOW up) | `VALVE Z3` + `PUMP` on; stop with **B3** again |
| **SPA (optional, if the port forward works)** | | |
| 19 | VS Code PORTS tab forwards `8180`; open `http://localhost:8180/` | SPA loads (needs the device's WiFi up — if it never loads, the device didn't join Wokwi-GUEST; that's a network issue, not the firmware) |

Note step 14 — exercising the **refused** long-press is the point: it proves the gate
won't let you dismiss a live hazard. That's the safety behavior, not a snag.
