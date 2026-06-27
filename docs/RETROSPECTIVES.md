# Tinkle — Retrospectives

Phase-end retrospectives, written by `/retro`. Most recent first.

<!-- Entries appended here at each phase boundary: velocity, scope changes,
     process notes, forecast update. -->

## Phase 5 — 2026-06-26 (Watchdog + integration)

**Points:** 32 / 32 planned (100%)
**Span:** 16 days (2026-06-10 → 2026-06-26) — **parallel with Phase 4** (identical window; the two phases were interleaved, so the throughputs split one calendar's output rather than describing two paces)
**Throughput:** 14.0 pts/calendar-week  ← headline (DEC-S026). Active companion: 10.7 pts/active-ISO-week.
**Estimate calibration:** 0 tasks re-estimated, net drift 0 pts  ← second clean-calibration phase in a row
**Sessions:** ~5 (in window, shared with Phase 4)   **PRs merged:** overlaps the Phase 4 set
**Issues:** 7 closed (#48–#52, #62, #77), 0 moved, 0 descoped

### Phase throughput line
| Phase | Date | Points | Span(d) | Throughput | Re-est'd | Net drift | Sessions | PRs |
|-------|------|--------|---------|------------|----------|-----------|----------|-----|
| 5 | 2026-06-26 | 32 | 16 | 14.0 pts/cal-wk | 0 | 0 | ~5 | ~7 |

### Retro notes
User skipped the worked/didn't/changed questions for this phase — process notes were captured in the Phase 4 retro (WiFi diagnosed twice → search session/docs sooner) and apply across both parallel phases. User requested the PM narrative as the qualitative record instead.

### Scope changes
- **#77 — DEC-019 phone-only pivot** added mid-phase: the first scope event that *removed* scope (deleted buttons + TM1637 + LED rings), promoting the SPA to the device's only interface. Done through the @architect gate; 9 docs reconciled, 14 dead tests pulled.
- **#77 points label** was missing — fixed to 5 (PROJECT_PLAN's documented estimate) at this retro.
- **Spillover rows reconciled:** PROJECT_PLAN's `3.x` (DEC-015 override) and `5.x` (DEC-014 self-test) were stale `[ ]` placeholders that actually shipped as #57 and #52 — marked `[x]` at this retro.

### PM read
Phase 5 closed 32 points across 7 issues in the now-familiar burst shape, only more extreme. Nineteen of those points — the entire two-key safety core (#48 ATtiny watchdog, #49 ESP32 heartbeat, #50 FaultManager, #52 the DEC-014 auto-return self-test) — landed in a single day-one burst on 06-10/11, shipped as one ~16-point coherent PR (Unit B) plus a 3-point follow. Then twelve quiet days, the phone-only pivot on 06-23, and a closing burst on 06-26 for the §17 acceptance stand-ins (#51) and the SPA-driven sim (#62). The 14.0 pts/calendar-week headline is real, but do not read it against Phase 4's 18.4: both numbers are carved from the *same* 16-day window — the two phases ran interleaved — so they split one calendar's output rather than describing two paces. Together the window produced ~74 points; Phase 5 is the safety-and-validation half. The pace story isn't the rate anyway. It's that the most safety-critical unit in the project — both halves of one heartbeat protocol, the second key of the fail-dry chain — went in fast and came out clean.

Second consecutive clean-calibration phase: zero re-estimates, zero net drift, every firmware issue landing on its planned label. The asterisk is #77, the DEC-019 phone-only pivot — it didn't exist at phase-scoping and went in mid-stream with its points label missing (fixed to 5 at this retro). So Phase 5, like Phase 4 with #72, absorbed an unplanned mid-phase task; the difference is direction. Every prior phase's scope event was an *addition* — Phase 1's toolchain absorbs, Phase 3's 8-point v1.4 valve rework. DEC-019 is the first that *removed* scope: it deleted the buttons, the TM1637, and the LED rings — real fabrication, parts, and enclosure-cutout work — to protect the deploy date, promoting the SPA to the device's only interface. A 5-point deletion task that subtracted far more than 5 points of downstream build. And it was done the right way: @architect gate first, one substantive correction taken (don't narrow SPEC's local-autonomy claim — retire DEC-006's narrower sub-claim instead), 9 docs reconciled, 14 now-dead tests pulled, build green.

Three things from the session files. First, and most important: @code-review caught a real fail-dry bug *in the watchdog itself* — the UnexpectedFlow resolved-condition was qualified with `isIdle()`, which defeated itself once latched (state reads Fault while water still pulses, so the gate reports "resolved" and clears the latch under live flow). On the one module the whole project exists to get right, the unit tests passed and the review tier caught it (PR #53, commit 46e6ba4). Worth noting the parallel Phase 4 retro filed this same catch under *its* ledger — shared window, easy to mis-attribute, but it's Phase 5's bug. The tiers have now earned their cost on fail-dry-class defects in the scaffolding (Phase 1) and the watchdog (here); keep them pinned to the safety layer. Second, the sim saga (#62) was the phase's real time sink and was never a code problem: a Wokwi custom chip that wouldn't compile gave way to an LEDC flow loopback, and then a recurring ~16–25s SPA drop — diagnosed only in Session 11 as the `net.forward` proxy exhausting the device's TCP pool — was fixed by buying the Private Wokwi Gateway, after a slow-poll code mitigation was tried and reverted. Infrastructure, not code; invisible until a session had been spent proving the firmware was fine. Third, a latent constraint is logged in three session files and still unguarded: if travel/settle timings ever shrink so the inter-run gap drops below HB_TIMEOUT_MS, queued runs share one armed heartbeat period and could trip HARD_MAX mid-queue. It's a comment, not a static_assert. Close it before someone tunes timings into it.

Forward: Phase 6, the bench build, is already materialized as 8 issues (#93–#100, 36 points), with 6.1 split into 6.1a–e against a rewritten step-by-step build-up doc. It's an interactive, hardware-paced phase — the bottleneck moves off the keyboard and onto a soldering iron and a shipping box, so throughput-per-week will fall hard, by design, not regression. Phase 5 already previewed this: #62 stayed open across three sessions as interactive-only validation no point total captures, because the run path now needs a human driving the SPA through the gateway in a browser. Carry three debts in. The heartbeat-encoding caveat above wants closing before bench timings get tuned. **[retro correction: the SPA *has* been hands-tested on the user's phone — the real ~375px target — so the visual is confirmed; the only residual gap is automated/CI 375px coverage, not a missing eyeball.]** And #51 (§17 acceptance) closed against LED/pulse stand-ins, not the wet bench — so the real acceptance still rides on Phase 6 hardware, regardless of the checkbox.

---

## Phase 4 — 2026-06-26 (Web API + SPA)

**Points:** 42 / 40 planned (105% — the +2 is #72, the fault-log doc-drift bug picked up mid-phase)
**Span:** 16 days (2026-06-10 → 2026-06-26)
**Throughput:** 18.4 pts/calendar-week  ← headline (DEC-S026 — first phase on this metric; not comparable to the retired h/pt of Phases 1–3). Active companion: 14 pts/active-ISO-week. Caveat: the 16-day window overlapped Phase 5 sim work, so this dilutes keyboard output (~26 PRs merged in-window, ~10 were Phase 4).
**Estimate calibration:** 0 tasks re-estimated, net drift 0 pts  ← point unit held perfectly
**Sessions:** 5 (in window)   **PRs merged:** 26 (calendar window; ~10 Phase 4)
**Issues:** 10 closed (#55–#59, #68–#72), 0 moved, 0 descoped; #90 filed as a deferred follow-up

### Phase throughput line
| Phase | Date | Points | Span(d) | Throughput | Re-est'd | Net drift | Sessions | PRs |
|-------|------|--------|---------|------------|----------|-----------|----------|-----|
| 4 | 2026-06-26 | 42 | 16 | 18.4 pts/cal-wk | 0 | 0 | 5 | ~10 |

### What worked
- Eventually able to get the sim to run; now familiar with what's possible simulation-wise.

### What didn't
- Diagnosing the WiFi was brutal — and we did it twice.

### Changes for next phase
- Search the session files and docs sooner for answers (the WiFi was diagnosed twice because the answer was likely already logged).

### Scope changes
- **#72** (fault-log doc-drift, 2 pts) picked up mid-phase — not a planned 4.x row; added to PROJECT_PLAN as 4.9 during this retro. Resolved by correcting spec §8 to the RAM-only reality (Option 2).
- **#90** filed (fault-log epoch-stamped NVS persistence, ~4–5 pt) — the deferred Option-1 feature, for a future phase.
- Context (Session 10, DEC-019 phone-only pivot): deleting the buttons / TM1637 / LED rings promoted the six-screen SPA to the device's **only** interface — it was built before it became load-bearing.

### PM read
Phase 4 is the first phase to close with the estimate model fully honest: 42 points, zero re-estimates, net drift zero — every issue's final `points:` label matched what PROJECT_PLAN guessed going in. After Phase 1 diverged ~24%, Phase 2 hit 100% only by absorbing a pre-built fertigation task, and Phase 3 ran 185% on unplanned valve rework, a phase that lands exactly where it was scoped is the real headline — not the 18.4 pts/calendar-week throughput. Read that throughput with a caveat, though: the 16-day span it's computed over was shared bumper-to-bumper with a whole second phase of Phase 5 sim work, so 18.4 pts/wk dilutes what the keyboard actually produced (~26 PRs merged in the window, only ~10 of them Phase 4). This is also the first phase on DEC-S026 throughput, so there's nothing in the same units to compare it against — Phases 1–3 quoted active h/pt. Treat it as the new baseline, not a trend.

The shape is the one every prior phase has worn: two dense bursts (~27 pts on 06-10/11, ~15 pts on 06-26) bracketing a two-week gap, with the only mid-phase work being the 06-15 @architect/DEC-018 gate. Scope inside Phase 4 was clean — 10 issues closed, none moved, none descoped — plus #72, the fault-log doc-drift bug, picked up mid-phase for 2 points. The genuinely large scope event in this window wasn't Phase 4's at all: the phone-only pivot (DEC-019, Session 10) deleted the buttons, the TM1637, and the LED rings, which quietly promoted Phase 4's six-screen SPA from "one of several interfaces" to the *only* interface the device has. The SPA happened to be built before it became load-bearing. Lucky — but it also means the SPA now carries weight it wasn't reviewed under.

Three patterns from the session files. First, #72 is the third consecutive phase where a source-of-truth doc contradicted the code — spec §8 listed the fault log as surviving power loss; it was RAM-only the whole time. Phase 1 had the C++-standard tier divergence, Phase 3 had v1.4 docs outrunning v1.3 firmware, now §8 promised a persistence that never shipped. That's not bad luck three times; it's a standing gap, and it earns a real habit: any PR touching a source-of-truth doc states what code is now in or out of compliance. Second, the merge-order trap fired in Session 9 — #61 stacked on #60 merged onto the dead Unit C branch instead of main (GitHub only re-targets when the base is *deleted*), forcing a re-deliver as #63. That's a cousin of Phase 1's #22 merge-race, and the second time a merge mechanic has put the wrong thing where it shouldn't be. Third, the verification tiers kept paying: @code-review caught a real fail-dry bug (the UnexpectedFlow clear-gate that defeated itself once latched), a lock-ordering bug, and two SPA contract bugs across the phase. Keep it on the safety-adjacent work.

On your three answers. "Eventually able to get the sim to run" — *eventually* is doing heavy lifting: a custom chip that wouldn't compile, then an LEDC loopback, then a paid Private Wokwi Gateway across three sessions just to get a connection that doesn't drop every 20 seconds. The familiarity is the durable asset there; the interactive-SPA + headless-wokwi-cli ladder is now a permanent capability, and that survives the phase. "Diagnosing the WiFi was brutal and we did it twice" — agreed, and the "twice" has a specific cost: the slow-poll mitigation got applied and then reverted before the gateway fix landed, so one of the two passes was thrown away. The real fix was infrastructure, not code — the kind of thing that's invisible until you've burned a session proving the code was fine all along. "Search the session and docs sooner for answers" — right instinct, worth pushing one step further: it only works if the docs are honest, and this is the same phase where §8 was actively lying about fault-log persistence. Pair it — search sooner *and* keep the docs trustworthy, or you'll just find the wrong answer faster. The clock-drift resolution this session (sim artifact vs. crystal-backed real `millis()`) is exactly the kind of answer that, written down once, kills a future re-debug.

Forward: the estimate model is finally calibrated — but it's calibrated on firmware. What's left (#51's §17 bench pass, the Phase 6 wet work) is all parts-gated and hardware-paced, so throughput-per-week will crater, not because anything slowed but because the bottleneck moves off the keyboard and onto a shipping box. Don't read that drop as a regression. Two debts to carry: the SPA has never been eyeballed at 375px (headless env, no @ui-reviewer, mock-preview only) and it's now the only interface the device has — close that before calling the UI done; and #90 (fault-log epoch-stamped NVS persistence, ~4–5 pt) is the one real piece of deferred feature work, so budget it rather than rediscover it.

---

## Phase 3 — 2026-06-10 (Flow + Calibration)

**Sessions:** 3 (Sessions 6–8)
**Points:** 24 / 13 planned (185% — 13 phase-labeled + 11 absorbed, see Scope changes)
**Wall clock:** 41.79h  (raw elapsed — includes two overnights + idle)
**Breaks:** 35.86h
**Active time (wall − breaks):** 5.93h ← honest headline
**Velocity:** 0.25 h/pt active  ← the only forecast number
**Issues:** 3 created (#34–#36), 3 closed, 0 moved.

**Metric model change (DEC-S013 rev):** first phase on `active = wall − breaks` with
transcript break inference; the per-PR dev/review split is retired (its anchors collapse
under burst-open/merge-whenever). Phases 1–2 quoted dev h/pt (0.12 / 0.10) — a different
metric. Do not blend; Phase 3 is the new baseline.

### Per-session breakdown
| Session | Date | Wall | Breaks | Active | Points | PRs |
|---------|------|------|--------|--------|--------|-----|
| 6 | 2026-06-07 | 0.17 | 0\* | 0.17 | 5 | #37 |
| 7 | 2026-06-08→10 | 38.92 | 33.64 | 5.28 | 14 | #42, #43, #45, #46 |
| 8 | 2026-06-10 | 2.70 | 2.22 | 0.48 | 5 | #47 |

\* inference: transcript-unavailable (session ran on another machine; 10-min session, no
material gap possible).

### Notes
Retro notes (what worked / didn't / changes) were skipped at Eric's request.

### Scope changes
- **Absorbed (11 pts, not planned for Phase 3):** v1.4 valve re-architecture task 1.8
  (8 pts, PR #42 — rework forced by PR #40 landing v1.4 docs while firmware was still
  v1.3), sim tooling `TINKLE_SIM`/`esp32_sim`/README (2 pts, PR #43), seeds-v4 template
  pull (1 pt, PR #46).
- 3.x flow-fault manual override (DEC-015, 3 pts) deliberately rides with the Phase 4
  settings API — not Phase 3 scope, still open as planned.
- Plan reorder approved (pending @architect): Phase 5 (watchdog) ahead of Phase 4
  (web/SPA), batched as Unit B = 5.1+5.2+5.3 (+5.x), Unit C = 4.1+4.2 (+DEC-015),
  Unit D = 4.3+4.4.

### PM read
Phase 3 closed all three planned tasks (#34, #35, #36 — 13 points) in a four-day window, but the headline number is 24 points in 5.93 active hours, because nearly half the phase's output was cross-phase absorption. The 0.25 h/pt active figure is the first reading on the new metric model (active = wall − breaks, per-PR dev/review split retired), so resist the urge to read it against Phases 1–2's 0.12/0.10 h/pt dev numbers — those measured a different thing, and the old split was demonstrably booking idle-waiting-on-merge as review anyway. Phase 3 is the new baseline, not a slowdown. The shape of the work stayed consistent with prior phases: short dense bursts inside long elapsed windows (Session 7: 5.28h active inside 38.92h wall).

The scope story is the one to say plainly: 11 of 24 points were unplanned absorbs, and 8 of those were the v1.4 valve re-architecture — a rework forced by PR #40 landing v1.4 *docs* while the firmware was still v1.3. That's the second consecutive phase where docs and implementation diverged on `main` (Phase 1 had the C++-standard tier divergence; this one had the spec itself outrunning the code). The rework went cleanly — fix-forward, no rollback, 72/72 green, fail-dry chain re-verified — but the lesson is that a docs-first decision needs an explicit "firmware now contradicts the authoritative spec" flag at the moment it merges, not a discovery two days later. Worth a habit: any PR that changes a source-of-truth doc states in its body what code is now out of compliance.

Two process patterns from the session files that the metrics flatten. First, environment drift bit twice in one session: CLAUDE.md claimed pio at `~/.local/bin` (absent), Task 1.8 shipped on a g++/Unity-shim substitute harness with an explicit "run real pio before merge" note, and the actual path turned out to be `~/.platformio/penv/bin/pio`. The substitute harness worked — but CLAUDE.md's Commands section is now wrong on the record and should be corrected before it misleads a future session. Second, review coverage loosened this phase: #43 was self-reviewed (defensible — build config and docs), and #47 — a fault-raising state machine — skipped @code-review entirely in favor of a direct user review. The user merged it in ~2.5h, which is fine as a one-off, but @code-review's track record here is two caught fail-dry bugs in Phase 1 and the boot-seeding nuisance-fault in #45. Skipping it on safety-adjacent modules is spending earned trust; I'd default it back on for the watchdog work.

Eric skipped the retro questions, so for the record, here's what I'd have wanted his read on: whether the v1.4 absorb felt like a planning failure or an acceptable cost of docs-first (I lean the latter, with the flag-on-merge fix above), and whether the Session 7 multi-machine toolchain detour (bee-grace → mil-dev, the apt/Click crash) is a recurring tax or now paid off — the README it produced suggests paid off.

Forward: the approved batch plan reorders Phase 5 (watchdog) ahead of Phase 4, pending the @architect check, and Unit B at ~16–19 points in one PR is the largest coherent unit attempted — roughly the size of all of Phase 2. The new splitting guidance explicitly permits this, but it also says larger units lean harder on a complete spec, crisp ACs, and the @architect gate; the watchdog is both halves of one heartbeat protocol *and* the second key of the fail-dry chain, so this is precisely the PR where the review tier should be at full strength, not relaxed. At Phase 3's planned-scope pace, Units B+C+D (~40–46 points) project to roughly 10–12 active hours — comfortably inside the Winter 2026–27 target, with absorbs remaining the only real schedule variable. They've appeared every phase so far; budget for them instead of being surprised a fourth time.

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

The clean read Phase 1 predicted: dev velocity held at 0.10 h/pt (vs Phase 1's noisy 0.12), but wall/pt fell 0.51 → 0.18 — far less idle. Review time is structurally tiny: #32 merged in 3 min, #33's merge landed after `/its-dead` (capped at session end), and the one large "review" block was a 30-min idle gap waiting on the #31 merge, correctly booked as a break, not review. Session 5 ran three PRs under the DEC-S015 per-PR-window model; the per-PR split attributes Clock ≈44 min dev, Scheduler ≈18 min, Fertigation ≈16 min of active work.

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
