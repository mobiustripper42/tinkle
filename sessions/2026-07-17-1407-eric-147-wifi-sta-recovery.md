---
session: 23
dev: eric
slug: 147-wifi-sta-recovery
branch: task/147-wifi-sta-recovery
started: 2026-07-17T14:07:40Z
ended:
points:
pr_numbers: [152]
status: open
transcript: /home/eric/.claude/projects/-home-eric-tinkle/c853843b-2b10-4953-8c85-20ddff80c02a.jsonl
---

# Session 23 — 147-wifi-sta-recovery

<!-- Task blocks appended by /kill-this, one per task. -->

## Task 1: Distributed cycle fires Zone 3 → 1 → 2 (diagnostic) + SPA Water tab

**Completed:**
- Diagnosed the "Zone 3 keeps faulting / under-watering" report from the History log: it's not Zone 3. Converting gallons→GPM shows Zone 3 steady at ~1.45 GPM in every run; **Zones 1 & 2 spike together to ~2.3–4.1 GPM on the first run(s) of some watering blocks, then settle to ~1.5**. Not a leak (two identical constant leaks is implausible), not line length (Z1 21 ft vs Z2 13 ft read nearly identical when spiked), and not clock-time morning (Jul 16 was normal all day; Jul 15 spiked in the afternoon). Under-determined trigger. Cheapest discriminating experiment: run Zone 3 *first* and see if the spike moves to it (→ firing position) or stays on 1 & 2 (→ the zones).
- `src/core/scheduler.cpp` — hardcoded Distributed fire order to Zone 3 → 1 → 2 (`kFireOrder{2,0,1}` in `evaluateDistributed`), `static_assert(MAX_ZONES==3)`, out-of-range indices skipped so each live zone fires exactly once. Temporary; reverted by #151.
- `test/test_native/test_main.cpp` — updated the one order-dependent test to expect 2,0,1. 154/154 native pass; esp32 + esp32_sim build.
- `web/index.html` — bottom nav `Sched` → **Water**; Watering page opens on the enabled mode's tab (guarded on `!schedDirty && !distDirty`); plan-preview line reads "Zone 3 → 1 → 2".
- Filed **#151** for the real feature (configurable per-zone order + enable/disable + UI, position-swap dropdowns) — deferred; the drag/dropdown UI and enable/disable were explicitly cut from this session.

**Code review:** 1 real UI regression fixed pre-merge — tab auto-select could flip the visible tab out from under a mid-Schedule-edit user; now gated on `!schedDirty && !distDirty`. Non-blocking: no test for the fire-order skip path at zoneCount 1/2 (deferred to #151).
**PR:** [#152](https://github.com/mobiustripper42/tinkle/pull/152)
**Points:** 3
**Branch:** task/dist-fire-order-diagnostic
**Opened at:** 2026-07-17T14:07:40Z

**Next Steps:**
- **Merge #152, then flash `main` from bee-grace** (`pio run -e esp32`, upload/OTA while idle) — the 3→1→2 order only takes effect on the box after a flash.
- **Run the experiment:** let a Distributed cycle fire (or force one), then read History. If the first-run flow spike now shows on **Zone 3**, it follows firing position; if it stays on **Zones 1 & 2**, it follows the zone. That result decides where to look next (shared tunnel-header purge/pressure vs. something per-channel).
- When done diagnosing, ship **#151** (configurable order + enable/disable) which reverts this hardcode.
- Still open from Session 22: #148 disposition (nightly-reboot backstop — close if #149 shows the disconnect gone), #140 (UI-review fixes), #144 (CalRange phantom).

**Context:**
- The flash still needs to include the already-merged Session-22 work (E2 fix #145, Distributed Watering #146+#150, WiFi STA recovery #149). Flashing `main` after #152 merges bundles all of it plus the diagnostic order.
- Zone numbering: Zone 1 = index 0, Zone 3 = index 2. "Zone 3 → 1 → 2" = indices {2,0,1}.
- The hardcode is Distributed-mode only; fixed schedule and manual runs are unaffected.
