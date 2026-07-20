---
session: 24
dev: eric
slug: 13-poopdeck-telemetry
branch: task/13-poopdeck-telemetry
started: 2026-07-20T03:43:19Z
ended: 2026-07-20T15:20:25Z
points: 3
pr_numbers: [156, 157]
status: closed
transcript: /home/estoffer/.claude/projects/-home-estoffer-tinkle/3ac48b6c-ad7c-4663-8732-baf7144d0fb2.jsonl
---

# Session 24 — 13-poopdeck-telemetry

<!-- Task blocks written manually — this session shipped via direct PR flow, not per-task /kill-this. -->

## Task 1: TVS part-number doc alignment (#154)

**Completed:**
- `docs/DECISIONS.md` — DEC-011 driver sourcing (276-277) and the DEC-020 headline (522) still named the per-FET TVS `1.5KE30A`; as-built is the bidirectional `1.5KE30CA` (established by #115). Aligned all three headline references; SMAJ30A original-sourcing history preserved. Last drift in the doc set — spec/BOM/buildup/wiring already said CA.
**PR:** [#156](https://github.com/mobiustripper42/tinkle/pull/156) — merged
**Points:** 1
**Branch:** task/154-tvs-part-number

## Task 2: Cal reject no longer latches/logs an unrelated in-flight run (#144)

**Completed:**
- Settled reachability: the race IS reachable. While cal is `Awaiting`, `RunController` is `Idle` and nothing gates a new run start on `cal.active()` — scheduler (`main.cpp` `scheduler.tick`) or manual `POST /api/run` (`api.cpp` `postRun`, which unlike `postStop` doesn't cancel cal) can move `state_` to Running. A then-submitted out-of-range number made `finish()` → `raiseFault(CalRange)` phantom-log a Faulted RunLog row with the unrelated run's stats + latch that healthy run.
- `calibration_controller.cpp` `finish()` — raise the CalRange latch only when `ownsRun || rc_.isIdle()` (`ownsRun = phase_==Running`, captured pre-mutation). Bad number still Rejected (no K write); wet-side latch withheld when it would hit an unrelated run.
- New native test `test_cal_reject_spares_unrelated_run`; header contract updated for the withhold. native 168/168, esp32 SUCCESS (70.8% flash).
- @code-review: no blockers, guard verified correct in every state (incl. a Settle-race edge), test confirmed as a real regression catch. Diagnostics-gap advisory (withheld reject leaves no fault-log trace) filed as **#158**.
**PR:** [#157](https://github.com/mobiustripper42/tinkle/pull/157) — merged
**Points:** 2
**Branch:** task/144-calrange-guard

**Next Steps:**
- **#158** — route the withheld CalRange reject through `FaultManager::note()` so it's visible in the fault log without latching. Needs a `FaultManager&` wired into `CalibrationController` (est 2).
- **Firmware versioning issue** (filed by eric this session) — `FW_BUILD` is `git rev-parse --short HEAD` only (`tools/fw_build_id.py:23`), so reflashes with a different baked `poopdeck_secret.ini` (gitignored build flag) show an identical SHA. A plain `-dirty` suffix won't help (gitignored file never dirties the tree); need build-config-aware identity — timestamp, build counter, or hash of resolved build flags.
- **#118** — History/clock "unsynced" — the poopdeck publisher refuses any run without a valid clock, so this directly gates telemetry completeness. Worth prioritizing.
- Still open from S23: check Zones 1 & 2 driver solder joints for the same marginal soldering that took Z3; #151 (configurable Distributed order, supersedes the #152 hardcode); #140 (UI-review basket); #110 clean rebuild.

**Context:**
- **Poop Deck telemetry (#13) is live end-to-end** — flashed `main`, box rebooted, a completed run landed in Grafana. The multi-flash confusion was two red herrings: (1) the baked MQTT password was wrong (copy-paste error — `mosquitto_pub … -d` returned CONNACK `rc:5` not authorised, proving broker reachable + creds bad, not a firmware issue); (2) the build SHA never changing across reflashes looked like a failed OTA but is expected — SHA = git HEAD, and only the gitignored secret changed. `{"flashed":true,"rebooting":true}` is the real "OTA took" signal, not the SHA.
- Broker is on bee-grace itself (`192.168.50.201:1883`, user `tinkle`, producer credential from poop-deck `deploy/.env`). Password is a compile-time build flag → any change needs rebuild + reflash, not a restart.
- Boot ring backfills logged runs on first MQTT connect, so history populates on reconnect, not just the next run.
- This session unblocked + merged #148 (nightly reboot) — resolved its stale merge conflicts (all both-add: main advanced with #145/#146/#149/#150/#152) as a merge commit (no rebase, per convention), verified 158/158 + esp32. Also merged #155 (#13). `main` now carries #13 + #148 + #144 + #154; that's the flashed image.
- Closed #138 (fixed by #145) and #142 (delivered by #146/#150; remainder in #151) — both were done-but-never-auto-closed.
- No `/kill-this` runs this session — work shipped via direct branch→PR→merge. Task blocks above written by hand at close so retro velocity isn't undercounted.
