---
session: 25
dev: eric
slug: its-alive-issue-163-yt56a5
branch: claude/its-alive-issue-163-yt56a5
started: 2026-07-24T11:19:39Z
ended:
points:
pr_numbers: [166, 168, 169]
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

## Task 2: RunLog records actual elapsed run time, not requested duration (#160)

**Completed:**
- `src/core/run_controller.{h,cpp}` — `logRun()` stored the *requested* `current_.durationSec`, so
  Stopped/Faulted runs (actual < requested) overstated flow time → skewed History + Grafana GPM low.
  Now records the ACTUAL Running dwell: a new `actualRunSec_`, frozen the instant a run leaves
  RUNNING via `freezeActualDuration()`, called from **both** exit paths — `enter()` (completion +
  stop) and `raiseFault()` (mid-run fault, which sets `state_ = Fault` directly, bypassing enter()).
  Zeroed in `resetRunMetrics()` so a fault-before-RUNNING logs 0 (no phantom). RunLog 11-byte format
  + telemetry v1 contract unchanged — no schema bump.
- `test/test_native/test_main.cpp` — two tests (Stopped + Faulted record actual < requested); the
  existing completed-run test still logs == requested. Suite **170/170**.
- History remediation left out per discussion: local ring self-purges at `RUNLOG_DEPTH=32`;
  poop-deck store cleanup is a manual server-side step the operator will do.

**Code review:** Clean bill of health — @code-review traced every run-exit path (completion, stop,
mid-run fault, DEC-023 abort, transition-state fault, idle fault), found no stale-value case, and
confirmed the swMax-capped path (the case the old code got most wrong) now logs correctly.
**PR:** [#168](https://github.com/mobiustripper42/tinkle/pull/168)
**Points:** 3
**Branch:** task/160-actual-elapsed-duration
**Opened at:** 2026-07-24T14:35:04Z

## Task 3: Firmware build versioning — sha+timestamp identity + per-build archive (#159)

**Completed:**
- `tools/fw_build_id.py` — `FW_BUILD` was the git short-sha alone, unchanged across reflashes
  that differ only in a gitignored build flag (re-baked `poopdeck_secret.ini`), so an OTA looked
  like it never took. Now `<sha>[-dirty]-<UTC yymmdd-HHMMSS>`: the timestamp changes it every
  flash and makes each archived filename unique. Refactored into pure, import-safe helpers
  (`compute_build_id` / `git_sha_and_dirty` / `archive_firmware` / `check_app_size`); post-build
  action copies `firmware.bin` → `build_archive/tinkle-<env>-<FW_BUILD>.bin` (gitignored). OTA
  slot-size gate preserved; archive-copy failures warn but never fail a good build.
- `tools/test_fw_build_id.py` (new) — 5 host tests, plain `python3` (no toolchain). **5/5.**
- `.gitignore` (`build_archive/`, `__pycache__/`); doc/comment sync across `platformio.ini`,
  `src/esp32/web_server.h`, `docs/OTA.md`, `docs/tinkle_firmware_spec.md`, `docs/DECISIONS.md`,
  `web/index.html` (all had said "git short-sha").

**Code review:** 1 robustness bug + 3 cleanups, all fixed — archive failure no longer fails the
build (try/except; SystemExit reserved for the size gate), stale platformio.ini comment, added
`git_sha_and_dirty` coverage, reworded dirty-flag docstring.
**PR:** [#169](https://github.com/mobiustripper42/tinkle/pull/169)
**Points:** 3
**Branch:** task/159-firmware-build-versioning
**Opened at:** 2026-07-24T16:12:12Z

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
