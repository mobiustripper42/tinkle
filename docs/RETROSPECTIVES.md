# Tinkle — Retrospectives

Phase-end retrospectives, written by `/retro`. Most recent first.

<!-- Entries appended here at each phase boundary: velocity, scope changes,
     process notes, forecast update. -->

## Phase 2 — 2026-06-06 (Persistence + Scheduler + Clock + Fertigation)

**Sessions:** 2 (Sessions 4 + 5; Session 3 was the boundary session — its 5 pts / PR #29 were the Phase-1-deferred #23 button model, counted in the Phase 1 retro)
**Points:** 16 / 16 (100%)
**Wall clock:** 2.92h
**Dev time:** 1.66h
**Review time:** 0.16h
**Velocities:**
- Wall: 0.18 h/pt
- Dev: 0.10 h/pt  ← headline forecast
- Review: 0.01 h/pt
**Issues:** 4 created (#25–#28, materialized in Session 3), 4 closed, 0 moved.

### Per-session breakdown
| Session | Date | Wall | Dev | Review | Breaks | Points | PRs |
|---------|------|------|-----|--------|--------|--------|-----|
| 4 | 2026-06-06 | 0.67 | 0.33 | 0.08 | 0.27 | 5 | #30 |
| 5 | 2026-06-06 | 2.25 | 1.33 | 0.08 | 0.78 | 11 | #31, #32, #33 |

### Notes
Retro notes (what worked / didn't / changes) and the PM read were skipped at Eric's request.

The clean read Phase 1 predicted: dev velocity held at 0.10 h/pt (vs Phase 1's noisy 0.12), but wall/pt fell 0.51 → 0.18 — far less idle. Review time is structurally tiny: #32 merged in 3 min, #33's merge landed after `/its-dead` (capped at session end), and the one large "review" block was a 30-min idle gap waiting on the #31 merge, correctly booked as a break, not review. Session 5 ran three PRs under the DEC-015 per-PR-window model; the per-PR split attributes Clock ≈44 min dev, Scheduler ≈18 min, Fertigation ≈16 min of active work.

### Scope changes
- **#28 (Fertigation) was ~90% pre-built** by #25–#27: the §6 policy (one fert run/day, `auto|on|off` override, day boundary) lives in the Scheduler (#27), and RunController already actuated the diverter per §4 (#25). #28 became "wire the cached diverter position across reboot (closing a deferred 2.1 flag) + lock the criteria with the first real Scheduler→RunController→ValveDriver end-to-end test." Delivered at its 3-pt estimate; flagged to Eric mid-task.
- **Schedule-entry NVS persistence deferred to Phase 4** (DEC-010) — no editor exists until the web-config API, so there is nothing to persist yet.
- **`swMaxRuntimeSec` carry-forward:** stored by Persistence but not read back into RunController until the Phase 4 settings API (the second of the two deferred 2.1 review flags; the diverter-cache flag closed in #28).
- New decisions this phase: **DEC-009** (Clock — local-epoch seam, TZ/DST in the ESP32 shim, hourly resync), **DEC-010** (Scheduler — IRunSink seam, forward-only minute-keyed idempotent eval, fert-slot-on-success).

### Process note
@code-review backend threw repeated 529 overloads during Session 5's Scheduler PR (#32) — two ~3.5-min hangs before being cut; that PR was reviewed inline instead, which caught a real forward-only idempotency bug. The backend recovered by the Fertigation PR (#33). Lesson logged in the session file: when the review agent is overloaded, review inline rather than burn wall-clock on retries.

## Phase 1 — 2026-06-04 (Actuation core)

**Sessions:** 2
**Points:** 25 / 25 Phase-1 scope (100%) + 6 spillover = **31 session-points** (Wokwi #7 [3, Phase 0] + C++11 fix [2] + TM1637 unblock [1])
**Wall clock:** 15.75h
**Dev time:** 3.58h
**Review time:** 1.75h
**Velocities** (per 31 session-points):
- Wall: 0.51 h/pt
- Dev: 0.12 h/pt  ← headline forecast (but a noisy baseline — see PM read)
- Review: 0.06 h/pt
**Issues:** 6 created (#9–#14), 6 closed, 0 moved to Phase 2. (Phase 0 #1–#8 also closed at this retro.)

### Per-session breakdown
| Session | Date | Wall | Dev | Review | Breaks | Points | PRs |
|---------|------|------|-----|--------|--------|--------|-----|
| 1 | 2026-06-03 | 7.67h | 1.00h | 0.42h | 6.25h | 16 | #15, #16 |
| 2 | 2026-06-04 | 8.08h | 2.58h | 1.33h | 4.17h | 15 | #17–#22, #24 |

### What worked
- "i got great instructions on getting wokwi running. pretty cool"

### What didn't
- "very low frustration level"

### Changes for next phase
- "i think we have a good flow at the moment, no changes"

### Scope changes
- **Unplanned, absorbed in-window:** C++11 build-standard fix + native/esp32 `-std=gnu++11` lockstep (PR #18, 2pts); TM1637 lib unblock — delisted `avishorp/TM1637` (PR #20, 1pt); Wokwi diagram pin-fix re-land after a #22 merge race (PR #24).
- **Cross-phase:** Wokwi #7 (Phase 0.7, 3pts) completed this window; #6 (toolchain) closed. Phase 0 (#1–#8) all closed at this retro — scaffold that predated session tracking.
- **Deferred:** #23 — 3-zone button model (DEC-006: each button runs its zone, any-button-cancels, long-press fault-clear). Needs @architect, and resolution **before** Phase 2 Persistence locks an NVS schema around zone count.

### PM read
Phase 1 landed all six planned tasks at their full 25 estimated points, plus 6 points of unbilled-but-real work that the plan didn't anticipate. That's the first thing worth saying out loud: the headline dev-time velocity of 0.12 h/pt is genuinely fast, but it's flattered by the point base. The 31 session-sum includes a Phase-0 Wokwi task (3), a C++11 standard fix (2), and a TM1637 lib stopgap (1) — none of which were Phase 1 scope on paper. Strip those and Phase 1 proper still came in clean, but the lesson for the velocity table is that "points completed" and "points planned" diverged by ~24% in the very first measured phase, almost entirely as toolchain and tier-alignment surprises. That's the baseline number to be skeptical of, not proud of.

The wall clock tells the more honest story. 15.75h elapsed against 3.58h of actual dev time and 1.75h of review — meaning 10.42h, two-thirds of the phase, was breaks and idle (Session 1 alone carried a 6.25h connection-drop gap). For schedule forecasting this matters more than the dev velocity: the 0.51 h/pt wall-clock figure is the one that predicts when work actually lands on the calendar, and it's 4x the dev rate. Don't let the 0.12 become the planning number for Phase 2. For an intermittent two-session phase, the cleaner read is throughput: ~25 real points across roughly two working days, with the work itself compressing into short dense bursts. The breaks aren't waste to scold — they're the shape of this workflow, and the estimate model should expect them rather than be surprised by them.

The verification tiers earned their keep, and that's the pattern I'd underline for the record. Code review caught a *real* fail-dry bug in each of the two scaffolding tasks — the loop rewrite dropping boot-time `pinMode(OUTPUT)` (actuator pins booting floating), and the buttons module firing a phantom long-press fault-clear on a boot-held button. Both are exactly the runaway-on / spurious-action class the whole architecture exists to prevent, and both passed the unit tests. Then the first-ever on-target `pio run -e esp32` immediately caught a C++11 aggregate-init break that the native tier had been compiling through at a newer default standard — the test tier had silently stopped being representative of the firmware. Pinning both tiers to gnu++11 closed that, and it's the single most valuable process artifact of the phase: a divergence that would otherwise have surfaced on hardware now surfaces in ~1s on the laptop. The tiers caught what the author didn't, repeatedly. That's the system working, and it's worth protecting — keep native and esp32 in C++-standard lockstep as the firmware spec note now says.

On the user's answers: "great instructions on getting Wokwi running" and "very low frustration" and "no changes" — I'll take the Wokwi enthusiasm at face value, it's a legitimately satisfying tier to get a full master→zone→pump→countdown→unwind sequence visible end to end. But "very low frustration / no changes" deserves a gentle prod rather than a nod, because the trenches say otherwise. This phase ate a flaky toolchain download (504s, IncompleteRead, a Ctrl-C-and-resume), a delisted TM1637 lib that broke `pio run` at resolution, a build-standard mismatch that shipped a latent bug into merged `#11`, and — the one I'd genuinely flag — a push-just-before-merge race where PR #22 captured the pre-fix snapshot and shipped broken Wokwi pins to `main`, needing a cherry-pick re-land via #24. None of that registered as frustration, which is good for morale and slightly concerning for vigilance: a phase with two caught fail-dry bugs and a merge race that put broken code on `main` is not a "no changes" phase. The merge-race in particular is a process gap, not a one-off — worth a habit of confirming the merge captured the intended HEAD before closing the issue.

Forward into Phase 2 (Persistence + Clock + Scheduler, 16 pts): the dependencies are already teed up cleanly — Clock (#2.2) feeds the display's `clockValid`/`HH:MM`, and Persistence (#2.1) retires the `BUTTON_RUN_SEC=600` placeholder with stored per-zone defaults. Two things to carry. First, the open design item #23 (3 zones, any-button-cancels, long-press fault-clear, DEC-006) conflicts with the shipped 2-zone + B3-stop button design and touches both hardware and the Buttons module — resolve it through @architect *before* Persistence locks a stored schema around zone count, or you'll migrate NVS twice. Second, Phase 2 is the first phase with no hardware-toolchain firsts left to absorb, so its velocity will be the first relatively clean read — treat Phase 1's numbers as a noisy baseline and expect Phase 2 to actually calibrate the estimate model. No timeline risk: the V1 build target is Winter 2026–27 and firmware has no hardware dependency, so the schedule pressure is real but not near.
