---
session: 27
dev: eric
slug: adopt-seeds
branch: task/adopt-seeds
started: 2026-08-24T17:45:40Z
ended: 2026-08-24T18:03:23Z
points: 1
pr_numbers: []
status: closed
transcript: /home/estoffer/.claude/projects/-home-estoffer-tinkle/ce93ff4c-1d19-4857-a136-97c75374b908.jsonl
---

# Session 27 — adopt-seeds

<!-- Task blocks appended by /kill-this, one per task. -->

## Task 1: Remote config change — distributed `perZoneMin` 28 → 22 (live device, no code)

Not a code task: no branch, no diff, no PR. eric was away from the house; this session ran on
bee-grace (192.168.50.201), on the same LAN as the controller, and changed a live setting.

**Done:**
- `POST /api/distributed` with `perZoneMin:22`, window 540–900 and `fertCount:1` carried over from
  a prior GET. The POST is a **full replace**, not a patch (`src/core/api.cpp:313-349`) — omitting
  `fertCount` would have silently zeroed it (`api.cpp:328`).
- Read-back confirmed: `perZoneMin:22`, `fertCount:1` intact, plan `cycles:3`, `runLenSec:440`,
  `cycleStartMin:[540,708,876]` — 22 min/zone/day as 3 runs of 7:20 at 09:00 / 11:48 / 14:36.
- **eric ran both curls, not this session** — `Bash(curl *)` is in the *deny* list
  (`.claude/settings.json:80`, deny block opens line 12; same in `~/.claude/settings.json`), and
  under DEC-S023 deny beats allow, so no allowlist entry would have helped. Approval to act does
  not unblock a denied tool; commands were handed over instead of reshaped.

**Points:** 1

**Next Steps:**
- **issue #179** (filed this session, `bug`) — the mask defect below. Decide the fix rather than
  patching reflexively; the two consequences may not want the same one.
- Confirm the three phantom missed-cycle rows actually landed. This session predicted them from the
  source and never verified them — `GET /api/status` on 2026-08-24 would have shown them stamped
  11:48 for zones 1/2/3. **Unverified prediction, not an observation.**
- Still carried from Session 25: `pio run -e esp32` unconfirmed on a toolchain machine; poop-deck
  Grafana alert rule on the `fault` field (issue #167 is the related liveness heartbeat).
- Seeds drift: this project is `seeds-version 4 vs 5` and owes a migration. Ten `logic` scripts are
  absent, including the DEC-S036 decisions toolchain (`check-decisions.mjs`,
  `gen-decisions-index.mjs`, `split-decisions.mjs`) — `docs/DECISIONS.md` is still a monolith while
  the CLAUDE.md shell already assumes one-decision-one-file. Reported by `drift.mjs`, not acted on.

**Context:**
- **A mid-day Distributed config save is not neutral — issue #179.** `setDistributed` assigns the
  config and nothing else (`src/core/scheduler.h:113`), while `distFiredMask_` is indexed by *cycle
  position* and resets only on a new calendar day (`scheduler.cpp:147`). Changing the cycle count
  re-points every surviving bit at a different wall-clock time. Two effects, both predicted from
  source before the write: today's 14:36 cycle was suppressed (bit 2 already set → `scheduler.cpp:152`),
  leaving the zones on 21 min; and `nextMissedCycle` judged the day's log against the *new* plan
  (`dist_summary.cpp:90-113`), finding the 11:48 slot elapsed-and-empty and stamping a Faulted entry
  per zone via the once-a-second idle-gated detector (`src/esp32/main.cpp:353-355`).
- **Expected artifacts on 2026-08-24 only, all self-clearing at midnight:** Home card reads "1 of 2
  cycles" with a behind-warning (then 1 of 3 after 15:00); three missed-cycle fault rows in History
  and in Grafana. Not a new fault — do not chase them.
- Device state at write time: idle, `queueDepth:0`, `fault.active:"none"`, clock valid 13:57 local,
  build `2945c10-260731-203907`. Fault log was all historical (most recent 2026-07-30).
- Sessions 25 and 26 were closed retroactively this session at eric's instruction; see the note in
  each file about what their `ended:` timestamps do and don't mean.
- Branch `task/adopt-seeds` was already merged (PR #178) when this session opened — the session is
  anchored to a spent branch. No code was written, so it never mattered; a code task would have
  needed a fresh branch off `main`.
