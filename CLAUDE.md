# Tinkle — Claude Code Project Context

## What We're Building

Tinkle is an automated drip-irrigation controller for Bay Branch Farm's high
tunnels — scheduled, per-zone watering with reliable manual override, replacing a
dumb smart-plug-on-the-pump. V1 runs the **Red Tunnel** (2 zones) plus a
general-purpose **hose-outlet zone** (Zone 3) and a Dosatron fertigation diverter —
three zone channels, one button each (DEC-006) — on a controller/board/enclosure
**sized for three tunnels**.

It is an embedded project: **ESP32 firmware** + an **ATtiny85 hardware watchdog** +
a **vanilla-JS phone UI** served from flash. Guiding philosophy — **fail dry**
(prevent runaway-on, never fear the missed cycle), **local autonomy** (no
network/cloud dependency), **build-for-three/populate-one**, **scheduled-now/
closed-loop-later**.

## Project Type

`tool` (embedded firmware + tiny static web UI). **Not a webapp** — no Supabase,
Next.js, React, or Playwright. The `@ui-reviewer` agent and `VersionTag.tsx` are
intentionally absent (gated out for `tool` type, DEC-S011 in seeds).

## Stack

- **Controller:** ESP32 DevKitC (38-pin), Arduino-ESP32 framework, PlatformIO.
- **Watchdog:** ATtiny85, separate binary (atmelavr), dependency-free.
- **UI:** vanilla HTML/CSS/JS SPA, gzipped into PROGMEM, served by
  ESPAsyncWebServer at `http://tinkle.local`.
- **Persistence:** NVS / Preferences.
- **Libs (starting set):** ESPAsyncWebServer + AsyncTCP, ArduinoJson, Preferences,
  a TM1637 driver, ESPmDNS, `configTime`/SNTP.
- **Build/test tiers (DEC-004):** native unit tests (host) → Wokwi sim (laptop) →
  breadboard bench (LED/pulse stand-ins) → wet hardware (final gate).

## Key Docs

| File | Purpose |
|------|---------|
| `docs/SPEC.md` | Scope — V1 in/out, the "Not V1" guardrail |
| `docs/DECISIONS.md` | Architecture decisions (DEC-NNN) |
| `docs/PROJECT_PLAN.md` | Phases, tasks, effort, velocity. Read at planning, written at retro. Current-phase tasks live in GitHub Issues. |
| `docs/RETROSPECTIVES.md` | Phase-end retros — written by `/retro` |
| `docs/AGENTS.md` | Agent + skill specs |
| `docs/VELOCITY_AND_POKER_GUIDE.md` | Estimation methodology |
| `docs/CHEATSHEET.md` | One-page skill reference |
| `docs/tinkle_v1_spec.md` | **Hardware spec** — water path, components, power, failure modes (source of truth for hardware) |
| `docs/tinkle_firmware_spec.md` | **Firmware spec** — modules, run state machine, API, constants (source of truth for behavior) |
| `docs/tinkle_wiring.html` | **Wiring + pin map** — source of truth for pins; if `pins.h` disagrees, the wiring doc wins |
| `sessions/*.md` (orphan `sessions` branch via `.sessions-worktree/`) | Per-session files (DEC-S013/S014) |
| `.claude/seeds-version` | Schema version for `/pull-seeds` |
| `.claude/project-type` | `tool` |

## Architecture

Single-core cooperative loop, **fully non-blocking** — no `delay()` in the run
path, target tick ≤ 10 ms. Long actions (valve travel, run durations) time against
`millis()`. Eleven modules (firmware spec §2); **`RunController` is the only module
allowed to command `ValveDriver` actuators** — every actuation flows through one
auditable sequence (§4).

**Two-key fail-dry chain (source control, no master — DEC-012):**
- **ESP32** *commands* each actuator (energizes the zone + diverter-leg FETs, enables
  the pump). There is no master valve.
- **ATtiny85** *arms* the safety relay (NO, energize-to-pass) that feeds 24V to the
  **pump** (the water source), armed only during a run. It commands nothing — it can
  only gate the pump's power.
- Water moves only when **both keys agree**. Lose either — power, heartbeat, the relay
  — and the pump de-powers → no source → no water, whatever the valves do. The valves
  rest closed (NC) as a convenience, not a safety barrier. See DEC-003/DEC-012.

Source layout: `src/esp32` (firmware), `src/attiny` (watchdog), `src/core`
(platform-independent logic, host-testable), `web/` (SPA source), `test/` (native).

## Commands

```bash
pio run -e esp32                 # build firmware
pio run -e esp32 -t upload       # flash ESP32
pio run -e attiny85              # build watchdog
pio test -e native               # host unit tests (src/core)
pio device monitor -b 115200     # serial monitor
```

PlatformIO CLI is installed (`~/.local/bin/pio`) and `pio test -e native` runs
green — its real prerequisite is the host C++ toolchain (`build-essential` /
`g++`), needed for the native env to build `src/core`. The ESP32 platform isn't
downloaded yet, so `pio run -e esp32` is still pending (Phase 0.6 / #6). Wokwi (VS
Code) runs the firmware in simulation without hardware (Phase 0.7).

## Conventions

- **Non-blocking always.** No `delay()` in the run path. Per-actuator timers so a
  diverter travel never blocks a zone's travel.
- **`pins.h` is the single pin source**, mirroring `docs/tinkle_wiring.html`. The
  wiring doc wins on pins; this doc wins on behavior.
- **`ValveDriver` is on/off — one FET per valve** (no H-bridges, no never-both-high). The
  safe/rest level is LOW (off): NC zones closed, NC fert-leg closed, NO bypass open (plain),
  pump off. Energize to actuate; an NC valve cap-returns closed on de-energize.
- **`RunController` is the sole actuator commander.** Everything else requests runs
  through it.
- **Fail dry on every fault and on power loss.** Entering any fault commands the
  safe state (pump off → all valve FETs de-energized) and latches. The pump-power gate
  (not a valve) is what makes it dry.
- **Platform-independent logic lives in `src/core`** so it compiles for both the
  ESP32 and the native test runner. ESP32-only code stays in `src/esp32`.
- **Constants in one place; bench-confirm the physical ones** (`ZONE_TRAVEL_MS`,
  `DIVERTER_TRAVEL_MS`, flow K) — defaults are seeds, not gospel (§15).
- **C++ style:** prefer `constexpr` over `#define` for typed constants; keep modules
  one translation unit each; comments explain *why*, not *what*.

## Testing

- **Native (host):** the load-bearing tier. Exercise the run state machine,
  scheduler, fert policy, flow math, and calibration with a **fake clock and fake
  GPIO** and injected faults. `pio test -e native`.
- **Sim (Wokwi):** full-firmware integration — virtual GPIO, TM1637, buttons, an
  injectable flow-pulse source.
- **Bench:** breadboard, LEDs standing in for valves/pump, a square-wave
  source faking the flow sensor. Runs the firmware spec **§17 acceptance checklist**.
- **Wet confirm:** real parts, real water — the final gate (Phase 6).

The §17 acceptance checklist is the definition of done for the fail-dry behavior.

## Session Skills

| Skill | When | What |
|-------|------|------|
| `/its-alive` | Session start | Ensure `.sessions-worktree/`, open session file on `sessions` branch, capture transcript, recommend task |
| `/pause-this` · `/restart-this` | Mid-session break / resume | Build check + WIP commit; reload context |
| `/kill-this` | Per task (DEC-S013) | Build check, commit, @code-review, open PR, append `## Task <N>` to session file |
| `/its-dead` | Session end (once) | Stamp `ended:`, tally points, close session file |
| `/start-phase` · `/retro` | Phase boundaries | Materialize phase as Issues / compute velocity, write retro, version-bump |
| `/push-seeds` · `/pull-seeds` | Workflow sync | Backport to / pull from the seeds templates via @sync-config (schema-version gated) |
| `/read-the-tape` | After a notable session | Audit JSONL for anti-patterns via @tape-reader |
| `/doc-consistency-check` | Before phase boundaries | Cross-reference doc claims via @doc-consistency (report-only) |

**Dev identity:** `~/.claude/devname` (one-line handle). Used in session filenames.
**Task model:** PROJECT_PLAN.md read at planning, written at retro; current-phase
tasks are GitHub Issues; a phase ends when its issues close.

## Agents

| Agent | Model | When | Purpose |
|-------|-------|------|---------|
| @architect | Opus | Before design decisions, new deps, scope creep | Coherence vs SPEC + DECISIONS |
| @code-review | Sonnet | After commits (wired into `/kill-this`) | Catch issues early |
| @pm | Sonnet | Session start/end | Track progress, flag risks |
| @sync-config | Sonnet | `/push-seeds`, `/pull-seeds` | Classify template-vs-project diffs |
| @tape-reader | Sonnet | `/read-the-tape` | Audit JSONL for anti-patterns |
| @doc-consistency | Sonnet | `/doc-consistency-check` | Cross-reference doc claims (report-only) |

(`@ui-reviewer` omitted — `tool` type.)

## Versioning

No `package.json`, so the version-bump steps in `/retro` / `/bump-major` no-op
silently. If V1 wants a version surface later, add a `VERSION` file + git tags and
revisit — not needed now.

## PR Workflow

- Each task gets a branch: `git checkout -b task/X.Y-short-description`.
- `/kill-this` opens the PR (`closes #N`), runs @code-review. Keep ≤3 open PRs.
- No `production` branch by default — PRs ship to `main`. Deployable projects can
  add a downstream `production` branch and ship with `/promote-production` (ff-merge
  `main` → `production`); only that skill gates on `origin/production` (DEC-S022).

## Approach to Action

Default to action. For non-trivial or destructive work, say what you're about to
do and why in a sentence, then proceed — don't stall for approval on local,
reversible, or diagnostic steps (builds, tests, file edits, sim runs). Reserve
explicit confirmation for the genuinely consequential: flashing hardware, force-
pushes, anything touching shared/remote state, anything hard to reverse.

Check `docs/SPEC.md` "Not V1" before adding scope. If a task feels bigger than its
estimate: stop, re-estimate; if it's a 13, break it down; if it's scope creep, flag
and move on.

## Tone

Occasional dry humor welcome. One good line beats three forced ones. Skip
disclaimers; be meticulous.

## Verbosity

End-of-turn summaries: one or two sentences — what changed, what's next. Don't recap
work just watched, don't restate the task. Mid-session updates: one sentence per
state change ("Found X." "Build green."). The session-summary block is dense, not
voluminous — cut the wall of prose.

## Cost and Waste

Never minimize cost. Banned phrasings (and any synonym whose function is to
minimize): "essentially zero", "negligible", "only a few cents", "just X dollars",
"a rounding error", "not a big deal", "don't worry about it". It's real money and
real resources — compute, parts, services, time. Waste of any kind is a fact, not a
problem to console about: acknowledge it and move on. No reassurance.
