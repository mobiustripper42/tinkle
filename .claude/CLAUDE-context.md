# Tinkle — Project Context

Everything specific to **this** project. The seeds-managed `CLAUDE.md` shell reads this file at session start and treats it as authoritative for project-specific facts (DEC-S019). This is a **`tool`** project (embedded firmware + a tiny static web UI), so the shell's webapp defaults — Playwright/pgTAP, Supabase migrations, 375px screenshots, `<VersionTag />`, `@ui-reviewer` — are overridden or N/A below. Nothing here syncs from seeds.

## What We're Building

Tinkle is an automated drip-irrigation controller for Bay Branch Farm's high tunnels — scheduled, per-zone watering with reliable manual override, replacing a dumb smart-plug-on-the-pump. V1 runs the **Red Tunnel** (2 zones) plus a general-purpose **hose-outlet zone** (Zone 3) and a Dosatron fertigation diverter — three zone channels, **phone-controlled** (DEC-019 made V1 phone-only; the per-zone buttons of DEC-006 were cut) — on a controller/board/enclosure **sized for three tunnels**.

It is an embedded project: **ESP32 firmware** + an **ATtiny85 hardware watchdog** + a **vanilla-JS phone UI** served from flash. Guiding philosophy — **fail dry** (prevent runaway-on, never fear the missed cycle), **local autonomy** (no network/cloud dependency), **build-for-three/populate-one**, **scheduled-now/closed-loop-later**.

## Project Type

`tool` (embedded firmware + tiny static web UI). **Not a webapp** — no Supabase, Next.js, React, or Playwright. The `@ui-reviewer` agent and `VersionTag.tsx` are intentionally absent (gated out for `tool` type, DEC-S011 in seeds).

## Stack

- **Controller:** ESP32 DevKitC (38-pin), Arduino-ESP32 framework, PlatformIO.
- **Watchdog:** ATtiny85, separate binary (atmelavr), dependency-free.
- **UI:** vanilla HTML/CSS/JS SPA, gzipped into PROGMEM, served by ESPAsyncWebServer at `http://tinkle.local`.
- **Persistence:** NVS / Preferences.
- **Libs (starting set):** ESPAsyncWebServer + AsyncTCP, ArduinoJson, Preferences, ESPmDNS, `configTime`/SNTP. (The TM1637 driver was dropped — DEC-019, phone-only.)
- **Build/test tiers (DEC-004):** native unit tests (host) → Wokwi sim (laptop) → breadboard bench (LED/pulse stand-ins) → wet hardware (final gate).

## Architecture

Single-core cooperative loop, **fully non-blocking** — no `delay()` in the run path, target tick ≤ 10 ms. Long actions (valve travel, run durations) time against `millis()`. Nine modules (firmware spec §2 — the `Buttons` + `Display` modules were cut by DEC-019); **`RunController` is the only module allowed to command `ValveDriver` actuators** — every actuation flows through one auditable sequence (§4).

**Two-key fail-dry chain (source control, no master — DEC-012):**
- **ESP32** *commands* each actuator (energizes the zone + diverter-leg FETs, enables the pump). There is no master valve.
- **ATtiny85** *arms* the safety relay (NO, energize-to-pass) that feeds 24V to the **pump** (the water source), armed only during a run. It commands nothing — it can only gate the pump's power.
- Water moves only when **both keys agree**. Lose either — power, heartbeat, the relay — and the pump de-powers → no source → no water, whatever the valves do. The valves rest closed (NC) as a convenience, not a safety barrier. See DEC-003/DEC-012.

Source layout: `src/esp32` (firmware), `src/attiny` (watchdog), `src/core` (platform-independent logic, host-testable), `web/` (SPA source), `test/` (native).

## Commands

```bash
pio run -e esp32                 # build firmware
pio run -e esp32 -t upload       # flash ESP32
pio run -e attiny85              # build watchdog
pio test -e native               # host unit tests (src/core)
pio device monitor -b 115200     # serial monitor
```

PlatformIO CLI lives at `~/.platformio/penv/bin/pio` (official `get-platformio.py` installer — NOT the apt package, whose click-8.1 incompatibility crashes; see README). `pio test -e native` and `pio run -e esp32` both run green; the native env's real prerequisite is the host C++ toolchain (`build-essential` / `g++`). Wokwi runs the firmware in simulation without hardware — build `esp32_sim` (`-D TINKLE_SIM`) for sim-shortened travel/run constants.

## Additional Docs

Project-specific docs beyond the baseline `## Key Docs` table in the `CLAUDE.md` shell. The hardware/firmware/wiring specs are **sources of truth** — defer to them:

| File | Purpose |
|------|---------|
| `docs/tinkle_v1_spec.md` | **Hardware spec** — water path, components, power, failure modes (source of truth for hardware) |
| `docs/tinkle_firmware_spec.md` | **Firmware spec** — modules, run state machine, API, constants (source of truth for behavior; §17 = the fail-dry acceptance checklist) |
| `docs/tinkle_wiring.html` | **Wiring + pin map** — source of truth for pins; if `pins.h` disagrees, the wiring doc wins |
| `docs/OTA.md` | **OTA runbook** — build, address-per-device, upload (curl/SPA), verify, guardrails, USB recovery |
| `docs/DRAFT-v1.4-BOM.md` | Draft v1.4 bill of materials |
| `docs/DRAFT-v1.4-valve-rearchitecture.md` | Draft v1.4 valve re-architecture proposal |

Baseline docs the shell lists that **don't apply here:** no `docs/BRAND.md` or `docs/USER_STORIES.md` (embedded tool). `docs/SPEC.md`, `docs/DECISIONS.md`, `docs/PROJECT_PLAN.md`, `docs/RETROSPECTIVES.md`, `docs/AGENTS.md`, `docs/VELOCITY_AND_POKER_GUIDE.md`, `docs/CHEATSHEET.md` are present per the baseline.

## Workflow Overrides

The shell's `## Micro Workflow` is webapp-shaped (Playwright + pgTAP + 375px screenshot). Tinkle's stack is firmware + watchdog + static SPA — those steps are replaced by the **build/test tiers (DEC-004)**:

- **Step 5 (Write the test):** native host tests against the run state machine, scheduler, fert policy, flow math, and calibration, with a **fake clock and fake GPIO** and injected faults. No Playwright, no pgTAP.
- **Step 6 (Run targeted tests):** `pio test -e native` (the load-bearing tier); escalate to Wokwi sim for full-firmware integration. Bench (breadboard, LEDs/square-wave stand-ins) runs the firmware spec **§17 acceptance checklist**; wet hardware is the final gate (Phase 6).
- **Step 7 (Mobile screenshot):** N/A — the UI is a tiny served SPA, eyeballed on a phone at `http://tinkle.local`.
- **Build check** (for `/kill-this`, `/pause-this`) = `pio test -e native` + `pio run -e esp32` green.
- **The §17 acceptance checklist is the definition of done for the fail-dry behavior.**

## Testing

- **Native (host):** the load-bearing tier. Run state machine, scheduler, fert policy, flow math, calibration — fake clock + fake GPIO + injected faults. `pio test -e native`.
- **Sim (Wokwi):** full-firmware integration — virtual GPIO, the SPA over a forwarded port, an injectable flow-pulse source (phone-only since DEC-019 — no TM1637/buttons in the diagram).
- **Bench:** breadboard, LEDs standing in for valves/pump, a square-wave source faking the flow sensor. Runs firmware spec **§17**.
- **Wet confirm:** real parts, real water — the final gate (Phase 6).

## Migration Protocol (project)

**N/A — no database.** Persistence is on-device **NVS / Preferences**; there is no Supabase, no migrations ledger. The shell's Supabase toolchain, `safe-supabase.sh` guard (DEC-S009), and Vercel env-sync don't apply.

## Conventions

- **Non-blocking always.** No `delay()` in the run path. Per-actuator timers so a diverter travel never blocks a zone's travel.
- **`pins.h` is the single pin source**, mirroring `docs/tinkle_wiring.html`. The wiring doc wins on pins; this doc wins on behavior.
- **`ValveDriver` is on/off — one FET per valve** (no H-bridges, no never-both-high). The safe/rest level is LOW (off): NC zones closed, NC fert-leg closed, NO bypass open (plain), pump off. Energize to actuate; an NC valve cap-returns closed on de-energize.
- **`RunController` is the sole actuator commander.** Everything else requests runs through it.
- **Fail dry on every fault and on power loss.** Entering any fault commands the safe state (pump off → all valve FETs de-energized) and latches. The pump-power gate (not a valve) is what makes it dry.
- **Platform-independent logic lives in `src/core`** so it compiles for both the ESP32 and the native test runner. ESP32-only code stays in `src/esp32`.
- **Constants in one place; bench-confirm the physical ones** (`ZONE_TRAVEL_MS`, `DIVERTER_TRAVEL_MS`, flow K) — defaults are seeds, not gospel (§15).
- **C++ style:** prefer `constexpr` over `#define` for typed constants; keep modules one translation unit each; comments explain *why*, not *what*.

## Versioning (project)

**No `package.json`**, so the shell's version-bump steps in `/retro` / `/bump-major` no-op silently. The `<VersionTag />` component is N/A (Next.js/Vercel). If V1 later wants a version surface, add a `VERSION` file + git tags and revisit — not needed now.

## PR Workflow (project)

Follows the shell, with one note: **no `production` branch by default** — PRs ship to `main`. Tinkle is a tool, not a deployed service; only `/promote-production` cares, and it gates on `origin/production` existing (DEC-S022). Stacking PRs is preferred for dependent tasks.

## Model Selection (project override)

Tinkle runs an **escalate-by-complexity** rubric — default low and step up by task length and complexity. **Fable is disabled (seeds DEC-S029)**, so the former three-tier rubric collapses to two; the Frontier tier returns if/when Fable is re-enabled.

| Tier | Model | Use for |
|------|-------|---------|
| Workhorse | `claude-sonnet-4-6` | **Default main session** and most agents. Single-file edits, scoped tasks, reviews. |
| Hard | `claude-opus-4-8` | The "stuck" escalation; **fail-dry / safety-chain logic**; architecture decisions (see `@architect`); long-horizon, multi-file work where holding coherence is the bottleneck. |

- **Reach for `effort` before a tier.** `xhigh` floor for coding/agentic work, `high` for intelligence-sensitive, `max` only when correctness must beat cost.
- **Spec up front, then let it run.** The firmware spec (`docs/tinkle_firmware_spec.md`) + the Issue's acceptance criteria **are** the spec — point the model at them rather than over-decomposing.
- **File memory is a force multiplier.** Session files, `docs/DECISIONS.md`, acceptance criteria — keep current, reference explicitly.
- **`@architect` runs `claude-opus-4-8`** (frontmatter authoritative — matches `.claude/agents/architect.md`). Reviewers stay Sonnet. New agents default Sonnet; add a `model:` line only for architecture-level reasoning.

## Approach to Action (project override)

**This overrides the shell's `## Approval Before Action` / `## Bug Reports & Questions` gates.** Tinkle defaults to action: for non-trivial or destructive work, say what you're about to do and why in a sentence, then proceed — **don't stall for approval on local, reversible, or diagnostic steps** (builds, tests, file edits, sim runs). Reserve explicit confirmation for the genuinely consequential: **flashing hardware, force-pushes, anything touching shared/remote state, anything hard to reverse.**

Check `docs/SPEC.md` "Not V1" before adding scope. If a task feels bigger than its estimate: stop, re-estimate; if it's scope creep, flag and move on. (The shell's *Splitting is a reviewability call* guidance applies as written.)

## Tone (project)

Per the shell: occasional dry humor welcome, one good line beats three forced ones. Skip disclaimers; be meticulous.
