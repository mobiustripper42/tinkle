---
session: 25
dev: eric
slug: its-alive-issue-163-yt56a5
branch: claude/its-alive-issue-163-yt56a5
started: 2026-07-24T11:19:39Z
ended:
points:
pr_numbers: [166]
status: open
transcript: /root/.claude/projects/-home-user-tinkle/f5b4c5ee-ffae-5178-9146-73ffa5ad5532.jsonl
---

# Session 25 — its-alive-issue-163-yt56a5

<!-- Task blocks appended by /kill-this, one per task. -->

## Task 1: Distributed-watering missed-cycle fault + Home day summary (#161, DEC-025)

Started as issue #163 (save fertigator valve cycles) — closed as a no-build: the diverters
are DEC-011 auto-return valves, so "hold state between runs" = keep them energized ~24h,
strictly worse than the per-run travel (already settled by DEC-021). Pivoted to #161.

**Completed:**
- `src/core/dist_summary.{h,cpp}` (new) — pure `computeDistSummary` (per-zone cycles
  completed vs planned + metered centigallons) + `nextMissedCycle` (oldest elapsed cycle+zone
  slot with no logged run → a ready-to-log Faulted RunEntry). #160-proof (never reads `durationSec`).
- `Fault::MissedCycle` + `RunController::logMissedCycle` / `lastRealRun` (`run_controller.h`).
  A missed cycle is logged as a Faulted run so it persists + telemeters through the existing
  Poop Deck publisher (`"fault":"missed-cycle"`) — no new publish path. Non-latching; never
  gates the pump. `lastRealRun` keeps the marker out of the operator-facing "last run".
- `src/esp32/main.cpp` — once-a-second, idle-gated detector (idle-gate prevents a miss push
  from shadowing a real run's telemetry in the same loop pass).
- `src/core/api.{h,cpp}` — additive `distSummary` status block; `faultName` → `missedCycle`.
  `src/core/telemetry_payload.cpp` — code 7 → `missed-cycle`.
- `web/index.html` — "Today · distributed" Home card + render + mock; **global font 18px→15px**
  (whole UI is `rem`-based). Gzip 17.9 KB (gate 50 KB).
- `docs/DECISIONS.md` DEC-025 + `docs/tinkle_firmware_spec.md` notes.
- 9 native tests; suite **177/177** (run via vendored Unity+ArduinoJson under `gnu++11` —
  PlatformIO registry is egress-blocked in this sandbox). esp32 build NOT runnable here
  (toolchain download policy-blocked) — flagged in the PR for a pre-merge `pio run -e esp32`.

**Code review:** @code-review found 4 integration-seam issues, all addressed — false "behind"
boundary (gate on cycle end), miss marker clobbering "last run" (`lastRealRun`), ring-eviction
duplicate (documented as accepted), and a same-pass double-push (already fixed by the idle gate).
**PR:** [#166](https://github.com/mobiustripper42/tinkle/pull/166)
**Points:** 5
**Branch:** claude/its-alive-issue-163-yt56a5
**Opened at:** 2026-07-24T14:12:01Z

**Next Steps:**
- Confirm `pio run -e esp32` + SPA gzip gate on a toolchain machine before merging #166.
- Downstream (poop-deck repo, deferred): Grafana alert rule on the `fault` field → email; a
  "no telemetry in N hours" heartbeat is the only way to learn of an outage while it's live.
- #161 rides behind the DEC-024 E2 discriminator gate like the rest of Distributed Watering.

**Context:**
- #163 closed as no-build (DEC-021 already settled it); no code shipped for it.
- Design converged over the session: missed cycle = a fault reported like any fault, modelled as
  a RunLog entry (not a FaultManager note, not a separate "summary" MQTT message) so it rides the
  existing run-telemetry path to Grafana. Non-latching so it never withholds future water.
