---
session: 21
dev: eric
slug: tvs-ca-doc-fix
branch: task/tvs-ca-doc-fix
started: 2026-07-09T17:27:25Z
ended:
points:
pr_numbers: [127]
status: open
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

**Next Steps:**

**Context:**
