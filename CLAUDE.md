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

PlatformIO CLI lives at `~/.platformio/penv/bin/pio` (official `get-platformio.py`
installer — NOT the apt package, whose click-8.1 incompatibility crashes; see
README). `pio test -e native` and `pio run -e esp32` both run green; the native
env's real prerequisite is the host C++ toolchain (`build-essential` / `g++`).
Wokwi runs the firmware in simulation without hardware — build `esp32_sim`
(`-D TINKLE_SIM`) for sim-shortened travel/run constants.

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
| @architect | Fable 5 | Before design decisions, new deps, scope creep | Coherence vs SPEC + DECISIONS |
| @code-review | Sonnet | After commits (wired into `/kill-this`) | Catch issues early |
| @pm | Sonnet | Session start/end | Track progress, flag risks |
| @sync-config | Sonnet | `/push-seeds`, `/pull-seeds` | Classify template-vs-project diffs |
| @tape-reader | Sonnet | `/read-the-tape` | Audit JSONL for anti-patterns |
| @doc-consistency | Sonnet | `/doc-consistency-check` | Cross-reference doc claims (report-only) |

(`@ui-reviewer` omitted — `tool` type.)

## Model Selection

Three tiers. Default low; escalate by **task length and complexity** — Fable 5's lead over Sonnet/Opus is smallest on short scoped tasks and widens the longer and more complex the work (state-machine reworks, cross-cutting refactors, long autonomous runs).

| Tier | Model | Use for |
|------|-------|---------|
| Workhorse | `claude-sonnet-4-6` | Default main session and most agents. Single-file edits, scoped tasks, reviews. |
| Hard | `claude-opus-4-8` | The "stuck" escalation; fail-dry/safety-chain logic; anything where being wrong is expensive but the task is bounded. |
| Frontier | `claude-fable-5` | Long-horizon, multi-file, high-autonomy work where holding coherence across the whole change is the bottleneck — and architecture decisions (see `@architect`). $10/$50 per MTok, 2× Opus both directions; reserve accordingly. |

- **Reach for `effort` before reaching for a tier.** `effort` (`low`/`medium`/`high`/`xhigh`/`max`, set via `output_config`) buys quality more cheaply than a model jump on a task the current model can already do. `xhigh` is the floor for coding/agentic work, `high` for intelligence-sensitive work, `max` only when correctness must beat cost. Fable 5 reaches production-quality code at *medium* effort and is more token-efficient than prior models — frontier quality does **not** require max effort.
- **Spec up front, then let it run.** Front-load the full task spec in one turn and let the model work long at high effort rather than over-decomposing a coherent task into tiny issues — Fable holds coherence across millions of tokens, and chopping the task throws that away. The firmware spec (`docs/tinkle_firmware_spec.md`) plus the GitHub Issue's acceptance criteria **are** the spec; point the model at them.
- **File memory is a force multiplier — ~3× more effective on Fable than Opus 4.8.** Session files, `docs/DECISIONS.md`, and acceptance criteria are exactly the persistent notes Fable exploits to improve its own output. Keep them current; reference them explicitly in the task.
- **Vision is a first-class input.** Fable 5 is state-of-the-art at vision — lean on it for the wiring doc, bench/breadboard photos, and datasheet figures instead of describing them in prose.
- **Silent fallback caveat.** Fable routes <5% of sessions (cyber / bio-chem / distillation classifiers, conservatively tuned) to Opus 4.8 automatically and tells you when it does. Defensive watchdog/fail-safe work won't trip it in normal use — but if a session unexpectedly feels a tier weaker, check for a fallback notice before chasing a phantom regression.
- **Agents:** model in agent frontmatter. `@architect` pins `claude-fable-5` — architecture decisions are where being wrong compounds, so they get the frontier tier. Reviewers (`@code-review`, `@pm`, `@doc-consistency`, `@tape-reader`) stay Sonnet.
- **New agents:** default to Sonnet. Add a `model:` line only when the agent's job is architecture- or vision-level reasoning.

## Versioning

No `package.json`, so the version-bump steps in `/retro` / `/bump-major` no-op
silently. If V1 wants a version surface later, add a `VERSION` file + git tags and
revisit — not needed now.

## PR Workflow

- Each task gets a branch: `git checkout -b task/X.Y-short-description`.
- `/kill-this` opens the PR (`closes #N`), runs @code-review. Keep ≤3 open PRs.
- **Stacking PRs is preferred** when tasks depend on each other. Branch the next
  task off the previous task branch (`git checkout -b task/X.Y-next task/X.Y-prev`),
  not off main.
- No `production` branch by default — PRs ship to `main`. Deployable projects can
  add a downstream `production` branch and ship with `/promote-production` (ff-merge
  `main` → `production`); only that skill gates on `origin/production` (DEC-S022).

## Workflow Notes

- **Never rebase a task branch that already has commits on origin.** If main has
  advanced while a PR branch is open, leave the branch as-is — GitHub's "Update
  branch" button handles this at merge time. Rebasing rewrites remote history and
  requires a force-push. Use `git merge --ff-only` only if explicitly asked.
- **JSON parsing in Bash:** Prefer `gh ... --jq '...'` (built-in jq via `gh`) or
  `jq` over `python3 -c "import json,sys; ..."` one-liners. The python invocations
  trigger per-pattern permission prompts (each unique argument list is a new
  allowlist entry), while `gh --jq` runs under the existing `Bash(gh ...)`
  allowance. For non-`gh` JSON, install/use `jq` directly. Reserve python for
  cases where the data shape genuinely needs control flow.
- **Bug reports:** create a GitHub issue, label `bug`, add to current or next phase.

## Approach to Action

Default to action. For non-trivial or destructive work, say what you're about to
do and why in a sentence, then proceed — don't stall for approval on local,
reversible, or diagnostic steps (builds, tests, file edits, sim runs). Reserve
explicit confirmation for the genuinely consequential: flashing hardware, force-
pushes, anything touching shared/remote state, anything hard to reverse.

Check `docs/SPEC.md` "Not V1" before adding scope. If a task feels bigger than its
estimate: stop, re-estimate; if it's scope creep, flag and move on.

**Splitting is a reviewability call, not a model-capability one.** Points size
*estimation*; they don't cap how much gets built in one run. Fable holds coherence
across far more than an 8, and splitting a *coherent* task fragments context — two
stitched-together 5s can land worse than one well-specified 8. So:
- **Don't split a coherent 8** (one feature, one migration, one subsystem) just to
  honor a ceiling — run it as one unit with the full spec up front.
- **Do split** when the diff is too large to review well, the blast radius or
  reversibility worries you, or an "8" is secretly two unrelated things.
- **Still break genuine 13s** — for review and risk, and because a 13 usually means
  the task isn't understood well enough yet. Not because the model can't hold it.
- Larger units lean harder on a complete spec + crisp ACs and the `@architect`
  gate. Raise the ceiling only with those in place.

## Tone

Occasional dry humor welcome. One good line beats three forced ones. Skip
disclaimers; be meticulous.

## Response Length

Default to the shortest response that fully answers — usually 2–5 sentences. No
preamble, no restating the question, no closing offers to help further. No
reflexive "let me know if you need more" or "happy to expand." Do offer concrete
follow-ups when they'd save a future round-trip. Length is requested explicitly
("expand," "give me the long version"), never the default.

## Verbosity

End-of-turn summaries: one or two sentences — what changed, what's next. Don't recap
work just watched, don't restate the task. Mid-session updates: one sentence per
state change ("Found X." "Build green."). The session-summary block is dense, not
voluminous — cut the wall of prose.

## Narration

`Response Length` and `Verbosity` above are the standing baseline. This is the
switchable knob on top of them — Opus 4.8 / Fable narrate more by default, so name
the level and I'll hold it for the session.

- **Terse** (default): Silence between tool calls. One sentence only when I find
  something, change direction, or hit a blocker. No "Now I'll…", "Let me check…",
  "Looking at…", no recapping what you just watched. Close with one or two
  sentences on the outcome.
- **Normal**: Brief progress notes at meaningful steps — not every action.
- **Narrate**: Explain reasoning as I go. For teaching, debugging, or watching a
  tricky change land.

Switch any time: `narration: terse|normal|narrate`.

Two mechanics move narration the same direction, independent of level:
- **Keep adaptive thinking on.** With thinking disabled, 4.8 / Fable spill
  reasoning into the visible answer — which reads as *more* narrative. Adaptive
  keeps reasoning in thinking blocks and the response clean.
- **Lower `effort`** (`low` / `medium`) trims preamble and confirmations — a
  coarser lever than the levels above.

## Cost and Waste

Never minimize cost. Banned phrasings (and any synonym whose function is to
minimize): "essentially zero", "negligible", "only a few cents", "just X dollars",
"a rounding error", "not a big deal", "don't worry about it". It's real money and
real resources — compute, parts, services, time. Waste of any kind is a fact, not a
problem to console about: acknowledge it and move on. No reassurance.
