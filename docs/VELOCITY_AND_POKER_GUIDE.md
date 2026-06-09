# Velocity Tracking & Scrum Poker — How-To Guide

A lightweight solo-dev process for tracking effort, estimating work, and knowing where you stand against a deadline. Built for CC-assisted development with fragmented time.

---

## Part 1: Velocity Tracking

### What it measures

**Velocity = active hours per effort point (active h/pt).** Active time is wall-clock minus idle: `active = wall_clock - breaks`. Lower is faster. It's the time you were actually at the keyboard on the work — and it's the only velocity number you forecast on.

### Where it comes from — you don't log anything

There is no tracker to maintain and no "log your hours" step. The numbers are extracted after the fact from data you keep anyway:

- Each session file records `started` and `ended` (stamped by `/its-alive` and `/its-dead`) plus the path to its transcript.
- `/retro` does the math at phase close: `wall_clock = ended - started`; `breaks` = idle gaps > 15 min inferred from the transcript; `active = wall_clock - breaks`; `active / points` = the velocity.
- Results land in two places: **`docs/RETROSPECTIVES.md`** (the source of truth — one block per phase, carrying the raw active hours + points so the number is always recomputable) and the **velocity table at the top of `docs/PROJECT_PLAN.md`** (an at-a-glance mirror).

If a number ever feels off, it's recomputable from `started`/`ended` + the transcript. Nothing depends on you remembering to write hours down — that's the whole point. The moment velocity becomes a logging chore it gets abandoned; here it's a read, not a chore.

### Your overall number, and across projects

For one phase, read RETROSPECTIVES.md. For your lifetime number — or a combined number across several repos — run the **velocity extractor** (`dev/claude/scripts/velocity.py`): it reads points off closed issues + active hours out of each repo's retros and prints lifetime active h/pt, a per-phase breakdown, and how much your per-session pace scatters.

**Do not average the per-phase h/pt numbers.** Correct overall velocity is `Σ active-hours ÷ Σ points`, not the mean of each phase's ratio — averaging ratios silently overweights small phases. The extractor sums them properly. Grep for a quick eyeball; run the extractor for the real number.

### Reading the numbers

- **`active / point` is the forecast number.** That's it. `wall_clock` is kept as raw bookkeeping (it includes overnight gaps and idle), but `wall / point` is *not* a velocity — don't quote it as one.
- **Ignore `Dev` / `Review` columns in older retros.** They came from a retired per-PR split that mis-attributed time on multi-PR sessions (it once reported more "review" hours than the session was even long). The `Active` figure in those same retros was always the real headline.
- **Velocity is project-shape-specific.** A Supabase CRUD app and an agent pipeline have very different active h/pt. Don't forecast one from the other's history.
- **Use the ramp, not your best-ever.** Early phases on a new project run high (dialing in the workflow) and settle later; a new project starts at the high end again, so quote the early band, not your record phase. (Phases predating the active-time model carry only a legacy `Velocity: X hrs/pt` — a different, older metric; don't blend it with active h/pt.)

### Rules of thumb

- **It only works if your pointing is consistent.** Velocity is a conversion factor between your estimate and clock time; if you point the same task a 3 on Tuesday and an 8 on Friday, velocity is measuring estimation noise, not speed. Consistent-but-biased pointing is fine — the bias is in both the history and the forecast, so it cancels. Random pointing is not. The extractor's per-session h/pt spread is the test: tight = your pointing holds; scattered = it doesn't.
- **Don't fudge.** The point is accurate forecasting, not looking fast.
- **Re-estimate when surprised** — if a 3 turns into an 8, update the plan. That's data, not failure.
- **Velocity stabilizes after ~10 sessions.** Before that, take projections with a grain of salt.

---

## Part 2: Scrum Poker (Estimation)

### The scale

**Fibonacci sequence: 2, 3, 5, 8, 13**

| Points | Meaning | Examples |
|--------|---------|----------|
| **2** | Small, well-understood, minimal unknowns | Add a column to a table, wire up an existing API endpoint, simple UI component |
| **3** | Moderate, clear approach, maybe one unknown | CRUD page with form validation, API client with auth + error handling |
| **5** | Significant work, some complexity or unknowns | Agent prompt design + structured output, data normalization pipeline, schedule board UI |
| **8** | Large, multiple moving parts, real unknowns | Full admin view with multiple interactive components, complex agent with business rules |
| **13** | Epic — probably should be broken up | Full auth system, complete write-back pipeline with error recovery. If you see a 13, ask: can this be two 5s and a 3? |

**No 1s** — if it's that small, just do it. Don't plan it.
**No 13s if avoidable** — break them down. A 13 means you don't understand the task well enough yet.

### How to poker (solo dev + Claude)

**Setup:** Claude proposes effort for each task in a phase. You review.

**The conversation:**
```
Claude: "Task 2.4 — Shift editing (split/merge/delete). I say 5."
You: "That's an 8. The merge logic has edge cases with gaps."
Claude: "Fair. What makes it an 8 vs a 5?"
You: "Merging two shifts means re-linking all the Xola event IDs 
      and recalculating crew requirements. It's not just UI."
Claude: "Agreed — 8. I'll note the merge complexity in the task description."
```

**Rules:**
1. **Disagree openly.** The whole point is catching misestimates before you're mid-build.
2. **Justify the gap.** If you say 8 and Claude says 3, one of you is missing something. Talk it out.
3. **The person doing the work wins ties.** You know your codebase and your constraints better than Claude does.
4. **Record disagreements.** PROJECT_PLAN.md has an "Estimation Poker — Standing Disagreements" table. If you can't resolve it, log it and revisit when you start the task.
5. **Re-estimate when surprised.** If a 3 turns into an 8 mid-session, update the plan. That's data, not failure.

### When to re-estimate

- **Before starting a new phase** — review all tasks, re-score anything that looks different now that you know more
- **After a task takes 2x+ the expected hours** — either the estimate was wrong or the task changed. Update the plan.
- **When cutting scope** — removed tasks get zeroed out, deferred tasks get marked, remaining estimates may shift

### Anti-patterns

- **Anchoring:** Don't let the first number stick without challenge. If Claude says 3 and you instinctively agree, pause and think about what could go wrong.
- **Sandbagging:** Don't inflate estimates to build in buffer. The velocity tracker already accounts for your actual pace — padding estimates just makes the projections useless.
- **Precision theater:** Don't debate whether something is a 4 or a 5. Pick the nearest Fibonacci number and move on. The scale is intentionally coarse.
- **Estimating during the build:** If you're mid-task and realize it's bigger, stop estimating and just finish it. Log the actual hours. Re-estimate the *next* similar task.

---

## Part 3: Putting It Together

### Rhythm

**Per session:** run `/its-alive` at the start and `/its-dead` at the end. That's the entire time-tracking obligation — those two stamps plus the transcript are everything `/retro` needs. No timer, no logging.

**Per phase boundary:** run `/retro`. It computes the per-session and phase active h/pt, writes RETROSPECTIVES.md, and updates the PROJECT_PLAN.md velocity table. If you're starting a new phase, do estimation poker (Part 2) on its tasks.

**When you want the big picture:** run the velocity extractor (Part 1) for your lifetime or cross-repo number. Check it against your remaining points — if projected active hours exceed the time you've got before a deadline, cut scope (PROJECT_PLAN.md has a cuttable-tasks list). The number doesn't lie.

### Cross-project tracking

Each project keeps its own RETROSPECTIVES.md and PROJECT_PLAN.md velocity table. Velocity is per-project and per-shape — don't average a CRUD app against an agent pipeline. When you want a combined view, the extractor takes multiple repo paths and reports each repo plus the properly-summed total (`Σ active ÷ Σ points`).

### The one thing that matters

**Point consistently and run the two stamps.** Everything downstream — the per-phase velocity, the forecasts, the cross-repo rollup — is recomputed from `started`/`ended` + the transcript, so there's nothing to log and nothing to forget. The only input that can quietly poison the number is inconsistent pointing, because velocity can't tell "I got slower" from "I pointed it lower." Keep your pointing honest and the system does the rest.
