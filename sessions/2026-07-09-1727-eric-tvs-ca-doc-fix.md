---
session: 21
dev: eric
slug: tvs-ca-doc-fix
branch: task/tvs-ca-doc-fix
started: 2026-07-09T17:27:25Z
ended: 2026-07-10T02:50:28Z
points: 16
pr_numbers: [127, 128, 129, 130, 134, 135]
status: closed
transcript: /home/eric/.claude/projects/-home-eric-tinkle/87d56e88-51f6-4ce0-83fd-d081424d8b36.jsonl
---

# Session 21 — tvs-ca-doc-fix

<!-- Task blocks appended by /kill-this, one per task. -->

## Task 1: Fix #124 — post-run drain grace (DrainGate) for both flow checks

**Completed:**
- New `src/core/drain_gate.h` — quiet-sub-window quiesce gate (≤2 pulses / 3 s, 60 s cap), shared by both detectors
- `src/core/flow_fault_detector.{h,cpp}` — idle unexpected-flow check waits out draindown on each IDLE entry; cap re-arms so a burst still latches (bounded at cap + one window)
- `src/core/valve_rest_monitor.{h,cpp}` — DEC-014 rest window opens after quiesce; never-quiet-by-cap flags the zone directly
- `test/test_native/test_main.cpp` — 4 new #124 tests + drain-aware timelines; 128/128 green
- `docs/tinkle_firmware_spec.md` — §7 + DEC-014 note + §15 `DRAIN_*` constants
- Also closed #119 (isolated ESP32 supply) and #121 (BOD doc) per bench confirmation

**Code review:** 1 finding, fixed — mute test wasn't reaching the post-unmute window under real drain defaults; now asserts liveness
**PR:** [#127](https://github.com/mobiustripper42/tinkle/pull/127)
**Points:** 3
**Branch:** task/124-drain-grace
**Opened at:** 2026-07-09T17:27:25Z

## Task 2: #126 — OTA firmware update over Wi-Fi (DEC-022)

**Completed:**
- Core: `RunController` OTA inhibit in the `IRunSink` seam (`requestRun` refuses mid-flash — covers scheduler + API); `Api::postOtaBegin`/`otaAbort` policy (IDLE **or** FAULT + empty queue)
- ESP32: streaming `/api/ota` in `src/esp32/web_server.h` (multipart→`Update.write()`, per-request ownership, optional `X-OTA-Key`), loop-side reboot, `build` sha in `/api/status`
- Tooling: `tools/fw_build_id.py` (git-sha inject + 1280 KB slot guard — image is 69%), `ota_secret.ini.example`
- SPA: Settings→Firmware upload control with progress + reboot/reconnect poll
- Docs: DEC-022 (incl. "already dual-slot, no repartition" + no-bootloader-rollback honesty), spec §10 + §17
- @architect pre-review reshaped the design (killed the repartition + rollback assumptions; added the mid-upload run-inhibit as the safety core)

**Code review:** 3 real bugs fixed pre-PR — raw-blob vs multipart wire mismatch (feature was DOA without it), overlapping-upload state clobber leaking the inhibit, cross-core restart-flag visibility
**PR:** [#128](https://github.com/mobiustripper42/tinkle/pull/128) — **stacked on #127**, base `task/124-drain-grace`
**Points:** 5
**Branch:** task/126-ota
**Opened at:** 2026-07-09T17:27:25Z

## Task 3: DEC-023 — watchdog never blocks the schedule (field false positive 2026-07-09)

**Completed:**
- Field event: spurious `FAULT_WATCHDOG` at end of run 1/3 at noon → queue dropped, day blocked. Trip can't be real (ATtiny trips only on 30-min ceiling); cause = unqualified single-sample GPIO36 read during the run-end tail (pump-relay switching transients)
- `src/core/watchdog.{h,cpp}` — `TRIP_CONFIRM_MS` (100 ms continuous) qualification + `tripConfirmed()`
- `src/core/run_controller.{h,cpp}` — `abortRun()`: confirmed trip aborts current run, logs Faulted, **preserves queue**, never latches; `raiseFault(Watchdog)` structurally refused; pre-open gate holds in PrepDiverter (wait-not-kill), skips one run past `WD_WAIT_MS` (60 s)
- `src/esp32/main.cpp` rewiring; spec §4/§9/§14/§15/§17; DEC-023 (relay is the safety, unchanged; wet-side faults keep their latch)
- Tests: 5 new + 3 rewritten, 135/135 green

**Code review:** 3 findings fixed — verdict preempted the PrepDiverter hold (would have reverted the gate to kill-on-first-confirm), stop-vs-abort log priority, Settle double-log
**PR:** [#129](https://github.com/mobiustripper42/tinkle/pull/129) — stacked on #128 (order: 127 → 128 → 129)
**Points:** 5
**Branch:** task/watchdog-nonblocking
**Opened at:** 2026-07-10T14:30:00Z

## Task 4: Zone 3 plain naming + ATtiny as-built fuse record

**Completed:**
- `web/index.html` — "Zone 3 — hose" → "Zone 3"; cal blurb "(hose outlet)" dropped
- `docs/tinkle_buildup.md` Step 8.0 — read-fuses-first + lfuse 0xE2 + **hfuse 0xDD (BOD 2.7V)** + as-built record: all 3 chips verified E2/DD/FF (2026-07-07), interchangeable
- `platformio.ini` — stale `board_fuses.hfuse = 0xDF` corrected to 0xDD; comments updated
- #121 was closed yesterday citing docs that hadn't landed — corrected on the issue; this PR is the real content

**Code review:** Docs/UI-string only, verified against Session 19 bench log
**PR:** [#130](https://github.com/mobiustripper42/tinkle/pull/130) — stacked on #129 (order: 127 → 128 → 129 → 130)
**Points:** 1
**Branch:** task/zone3-fuse-docs
**Opened at:** 2026-07-10T14:30:00Z

## Task 5: SPA — firmware build sha on the Home device card

**Completed:**
- `web/index.html` — "Firmware" row on the Device card reading `S.build` (+ mock `build:'mock0000'`); Settings → Firmware keeps its copy
- Also this window: Tiller proposal PRs converted to issues — #114→#131 (armed-rail sense, ~DEC-024), #80→#132 (volume dosing, ~DEC-025, corrections for shipped DEC-018 + #124 interplay), #65→#133 (flow-signature health, ~DEC-026, overlap-with-#132 + poop-deck reporting note); all with stale-numbering fixes

**Code review:** trivial UI row, build green
**PR:** [#134](https://github.com/mobiustripper42/tinkle/pull/134) — base main (127–130 merged by Eric)
**Points:** 1
**Branch:** task/spa-build-footer
**Opened at:** 2026-07-10T15:30:00Z

**Next Steps:**
- Flash from bee-grace off main (after #134 merges, or before — Settings shows the sha either way): `pio run -e esp32 -t upload --upload-port /dev/ttyUSB0`. Last cable flash — OTA onboard after.
- Post-flash: DEC-015 flow override OFF; wet run a zone; expect clean end-of-run (no UnexpectedFlow / valve-rest flag) — #124 acceptance.
- Watch History for `Faulted watchdog` entries — DEC-023 keeps trips visible but non-blocking.
- Parked, user-approved: #131 armed-rail sense (needs 2 divider R + series R + clamp), #132 volume dosing, #133 flow-signature health (+ future poop-deck reporting).

## Task 6: OTA operator runbook

**Completed:**
- `docs/OTA.md` — build anywhere, per-device address table (`http://tinkle` = router DNS for Android; `tinkle.local` = mDNS for bee-grace — NOT interchangeable), curl (`-F` multipart required) + SPA upload paths, sha verification, guardrail-refusal meanings, USB recovery
- `.claude/CLAUDE-context.md` — Additional Docs row
- Written from today's successful first OTA (bee-grace curl → `tinkle.local`)

**Code review:** docs only, commands verified against the live procedure
**PR:** [#135](https://github.com/mobiustripper42/tinkle/pull/135)
**Points:** 1
**Branch:** task/ota-runbook
**Opened at:** 2026-07-10T02:40:00Z

**Next Steps (session-wide):**
- **Watch tomorrow's scheduled runs** — the real acceptance for this session's firmware: DEC-015 flow override is now OFF (Eric unchecked "Disable flow checks" post-flash). Expect: all zones complete, clean end-of-run (no UnexpectedFlow / valve-rest flags — #124 DrainGate), and no watchdog latch can block the day (DEC-023). If anything faults, History tells the story (Faulted entries + build sha).
- Merge #135 (OTA runbook) if still open.
- Parked, approved backlog: #131 armed-rail sense (needs resistors), #132 volume dosing, #133 flow-signature health (+ poop-deck reporting fast-follow).

**Context:**
- Firmware on device: USB-flashed from main (post 127–130 merge), then #134 delivered via the FIRST OTA (bee-grace curl → tinkle.local). The cable is now optional; docs/OTA.md is the runbook.
- Address quirk (also memorized): `http://tinkle` = router DNS (Android/Windows); `tinkle.local` = mDNS (bee-grace/Windows). Neither works everywhere.
- Wall clock spans an evening gap (opened 17:27Z 7-09, resumed 7-10) — retro's break inference handles it.
- #121 was closed a day early on a wrong claim (docs hadn't landed) — corrected via PR #130 + issue comment. Lesson: verify "done" against the tree before closing an issue.
