# Tinkle — Project Plan

**Start date:** 2026-05-30
**V1 target:** Build target Winter 2026–27, running the 2027 season. Firmware has no
hardware dependency (DEC-004) — software phases proceed now; wet validation is
parts-gated.
**Critical path:** Actuation core + fail-dry chain (Phases 1, 5) proven on the bench
before any wet run.

---

## Estimation Method

Fibonacci scale (2, 3, 5, 8, 13). See `VELOCITY_AND_POKER_GUIDE.md`.
All estimates from planning poker between Eric and Claude.
Tests are baked into every task estimate — no separate testing tasks.

**Velocity** (per `/retro`, DEC-S013 — `dev_time` is the forecast number; wall clock is break-inflated):

| Phase | Sessions | Points | Wall (h) | Dev (h) | Review (h) | hrs/pt (dev) |
|-------|----------|--------|----------|---------|------------|--------------|
| 0 | — | ~19 | — | — | — | scaffold — done pre-session-tracking, no time data |
| 1 | 2 | 31\* | 15.75 | 3.58 | 1.75 | 0.12 |
| 2 | 2 | 16 | 2.92 | 1.66 | 0.16 | 0.10 |

\* 31 = 25 Phase-1-labeled (#9–#14) + 6 spillover (Wokwi #7 [3, Phase 0], C++11 build-standard fix [2], TM1637 lib unblock [1]). Wall clock was ~66% breaks (toolchain download, a connection-drop gap), so treat Phase 1 as a **noisy baseline** — Phase 2 is the first clean read. Full detail in `RETROSPECTIVES.md`.

---

## Phase 0: Scaffold & contracts

Everything needed to develop safely off-hardware. No watering behavior yet.

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 0.1 | Repo, git, GitHub remote, `.gitignore` | 2 | [x] [#1](https://github.com/mobiustripper42/tinkle/issues/1) |
| 0.2 | Seeds workflow install (skills, agents, `project-type=tool`, `seeds-version`) | 2 | [x] [#2](https://github.com/mobiustripper42/tinkle/issues/2) |
| 0.3 | Docs: CLAUDE.md, SPEC, DECISIONS, PROJECT_PLAN; source specs vendored to `docs/` | 3 | [x] [#3](https://github.com/mobiustripper42/tinkle/issues/3) |
| 0.4 | `platformio.ini` (esp32 / attiny85 / native) + `src/{esp32,attiny,core}` layout | 3 | [x] [#4](https://github.com/mobiustripper42/tinkle/issues/4) |
| 0.5 | `pins.h` from `tinkle_wiring.html` §B | 2 | [x] [#5](https://github.com/mobiustripper42/tinkle/issues/5) |
| 0.6 | Install PlatformIO; `pio run -e esp32` and `pio test -e native` green | 3 | [x] [#6](https://github.com/mobiustripper42/tinkle/issues/6) |
| 0.7 | Wokwi sim config — boot the firmware in simulation | 3 | [x] [#7](https://github.com/mobiustripper42/tinkle/issues/7) |
| 0.8 | Sessions branch + `.sessions-worktree/` | 1 | [x] [#8](https://github.com/mobiustripper42/tinkle/issues/8) |

**Phase 0 total: 19 pts** — complete (most done at scaffold; #6 toolchain + #7 Wokwi completed in Session 2; all closed at the Phase 1 retro)

**Ejection point:** A clean repo that builds for both MCUs, runs host tests, and
boots in sim — every later phase is faster and safer.

---

## Phase 1: Actuation core

The safety-critical heart. Bench-testable on LEDs/scope; no water.

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 1.1 | `ValveDriver` — latching pulse open/close, never-both-high invariant | 5 | [x] [#9](https://github.com/mobiustripper42/tinkle/issues/9) · §5 · *valve scheme superseded by 1.7 (DEC-011/012/013) — on/off FETs, no H-bridge, no never-both-high* |
| 1.2 | `ValveDriver` — diverter travel, master FET, pump relay, safe state | 3 | [x] [#10](https://github.com/mobiustripper42/tinkle/issues/10) · §5 |
| 1.3 | `RunController` state machine (IDLE→…→SETTLE, fault unwind) | 8 | [x] [#11](https://github.com/mobiustripper42/tinkle/issues/11) · §4; src/core, fake clock/GPIO |
| 1.4 | Non-blocking cooperative loop scaffold (tick ≤10ms, per-actuator timers) | 3 | [x] [#12](https://github.com/mobiustripper42/tinkle/issues/12) · §2 |
| 1.5 | Buttons — debounce, edge events, single-active invariant | 3 | [x] [#13](https://github.com/mobiustripper42/tinkle/issues/13) · §11 |
| 1.6 | TM1637 display — idle clock / MM:SS countdown / fault code | 3 | [x] [#14](https://github.com/mobiustripper42/tinkle/issues/14) · §12 |

**Phase 1 total: 25 pts** — complete (all closed). Ejection point met: full master→zone→pump→countdown→unwind sequence runs button-driven in the Wokwi sim (#7).

**Unplanned work absorbed in the Phase 1 window** (not in the original plan):
- C++11 build-standard fix + native/esp32 `-std=gnu++11` lockstep — [PR #18](https://github.com/mobiustripper42/tinkle/pull/18) (2 pts) · *Added during P1 retro*
- TM1637 lib unblock (delisted `avishorp/TM1637`) — [PR #20](https://github.com/mobiustripper42/tinkle/pull/20) (1 pt) · *Added during P1 retro*
- Wokwi diagram pin-fix re-land (#22 merge race) — [PR #24](https://github.com/mobiustripper42/tinkle/pull/24) · *Added during P1 retro*

**Deferred design item:** [#23](https://github.com/mobiustripper42/tinkle/issues/23) — 3-zone button model (each button runs its own zone, any-button-cancels, long-press fault-clear; DEC-006). Needs @architect + resolve **before** Phase 2 Persistence locks an NVS schema around zone count.

**Reopened by the v1.4 valve re-architecture (DEC-011/012/013) — not in the original Phase 1 total; needs GitHub issues:**

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 1.8 | `ValveDriver` + `RunController` + `pins.h` to the on/off model — one FET per valve (no H-bridge, no `IN1/IN2`, no never-both-high), **master removed**, diverter = two leg FETs, `pulseOpen/Close`→`openZone/closeZone` + `setDiverter(fertigate)`, drop `MASTER_FET`/`div_pos` cache, `ZONE_TRAVEL_MS`≈10s, re-map pins, update native tests | 8 | [x] · §3/§4/§5; DEC-011/012/013 |
| 3.x | Flow-fault manual override (DEC-015) — settings flag + `/api/settings` + SPA banner; `FlowMonitor` honors it | 3 | [ ] · §7/§10; lands with Phase 3/4 |
| 5.x | Auto-return self-test (DEC-014) — periodic valve-rests-closed check; flags a stuck/degraded valve | 3 | [ ] · §14; correctness, not a safety barrier |

Scope note: v1.4 sourcing settled on a 2-wire **auto-return on/off** valve (not the reverse-polarity hold-position part v1.3 assumed), so 1.8 was a real `ValveDriver` re-architecture, not the rename it was first scoped as (3→8). The §4 run sequence already gated on `zoneBusy()`, so the `RunController` seam survived; the driver + pin map underneath changed, and the master open/close states dropped. Done: `src/core/valve_driver.*`, `run_controller.*`, `persistence.*` (div_pos cache out), `src/esp32/pins.h` + `main.cpp`, the ATtiny comment, the Wokwi `diagram.json`, and the native tests (72 green). (Renumbered from 1.7 — Phase 1.7 was the 3-zone button model, #23.)

**Ejection point:** A simulated/bench run sequences zone→pump and unwinds
to safe state, driven by a button, with a live countdown.

---

## Phase 2: Persistence + Scheduler + Clock

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 2.1 | `Persistence` — NVS read/write of all stored state, write-on-change | 5 | [x] [#25](https://github.com/mobiustripper42/tinkle/issues/25) · §8 <!-- completed 2026-06-06 --> |
| 2.2 | `Clock` — NTP sync + free-running fallback | 3 | [x] [#26](https://github.com/mobiustripper42/tinkle/issues/26) · §13 <!-- completed 2026-06-06 --> |
| 2.3 | `Scheduler` — entry model, per-minute eval, queue, overlap guard | 5 | [x] [#27](https://github.com/mobiustripper42/tinkle/issues/27) · §13 <!-- completed 2026-06-06 --> |
| 2.4 | Fertigation policy — one fert run/day + per-entry override | 3 | [x] [#28](https://github.com/mobiustripper42/tinkle/issues/28) · §6 <!-- completed 2026-06-06 --> |

**Phase 2 total: 16 pts** — complete (all closed). §6 fert policy + diverter actuation/persistence landed across #25–#28; the `Scheduler` is the single eval engine (DEC-010), the `Clock` an NTP/free-run seam (DEC-009). Carry-forward: `swMaxRuntimeSec` stored-not-read-back until the Phase 4 settings API.

---

## Phase 3: Flow + Calibration

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 3.1 | `FlowMonitor` — ISR pulse count, rate window, gallons from K | 5 | [#34](https://github.com/mobiustripper42/tinkle/issues/34) · §7 |
| 3.2 | Fault detection — no-flow (grace) + unexpected idle flow | 3 | [#35](https://github.com/mobiustripper42/tinkle/issues/35) · §7, §14 |
| 3.3 | Calibration mode — start/finish endpoints, K to NVS, sanity bounds | 5 | [#36](https://github.com/mobiustripper42/tinkle/issues/36) · §7 |

**Phase 3 total: 13 pts**

---

## Phase 4: Web API + SPA

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 4.1 | ESPAsyncWebServer + STA-join/SoftAP fallback + mDNS | 5 | §10 |
| 4.2 | REST endpoints (status, schedule, settings, run, stop, calibrate, fault) | 8 | §10; range-validate, FAULT-gate |
| 4.3 | SPA — 6 screens, vanilla, mobile-first, graceful degrade | 8 | §10.1; dev vs mock API |
| 4.4 | gzip-embed pipeline (`build-spa`) → PROGMEM, < 50 KB | 3 | DEC-002 |

**Phase 4 total: 24 pts**

---

## Phase 5: Watchdog + integration

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 5.1 | ATtiny85 sketch — heartbeat edge-detect, two trip conditions, fail-dry default | 8 | §9; DEC-003 |
| 5.2 | ESP32 `Watchdog` — heartbeat emit (active runs only), trip-line read, force safe | 5 | §9 |
| 5.3 | Safety relay wiring + `FaultManager` integration | 3 | §14 |
| 5.4 | §17 acceptance checklist — full pass on the bench | 5 | LED/pulse stand-ins |

**Phase 5 total: 21 pts**

---

## Phase 6: Bench validation + wet confirm

Parts-gated (Winter 2026–27). Does not block Phases 1–5.

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 6.1 | Breadboard bring-up — real ESP32 + ATtiny + IRLZ44N valve FETs, LED/pulse stand-ins | 5 | |
| 6.2 | Confirm `ZONE_TRAVEL_MS` / `DIVERTER_TRAVEL_MS` against real parts | 3 | §15 |
| 6.3 | Calibrate flow K empirically (bucket test) | 3 | §7 |
| 6.4 | Wet confirm — full water run, all faults, fail-dry chain | 5 | §17 |

**Phase 6 total: 16 pts**

---

## Velocity Table

| Phase | Actual Hours | Effort Points | Hrs/Pt | Notes |
|-------|-------------|---------------|--------|-------|
| 0 | — | 19 | — | |
| 1 | — | 25 | — | |

**Lifetime velocity:** — hrs/pt

---

## Estimation Poker — Standing Disagreements

| Task | Claude says | You say | Question |
|------|------------|---------|----------|
| — | — | — | — |

---

## Phase Boundary Checklist

1. `pio test -e native` green; sim/bench checks for the phase pass.
2. @pm phase retrospective — velocity check, timeline update.
3. Write retrospective entry in `docs/RETROSPECTIVES.md`.
4. Return to planning chat — review docs against intent.

---

## Cuttable Tasks (if behind)

| Task | Why it's cuttable | Defer to |
|------|------------------|---------|
| 1.6 TM1637 display | Status nicety; web UI shows the same countdown | later in V1 |
| 4.3 SPA polish | API + a minimal page is enough to operate; full 6-screen polish can trail | later in V1 |
