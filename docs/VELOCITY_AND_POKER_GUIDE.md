# Velocity Tracking & Scrum Poker — How-To Guide

A lightweight solo-dev process for tracking effort, estimating work, and knowing where you stand against a deadline. Built for CC-assisted development with fragmented time.

---

## Part 1: Velocity Tracking

### What it measures

**Velocity = hours per effort point (hrs/pt).** Lower is faster. Track it per session, watch the trend.

### The workflow

**During a CC session:**
1. Note your start time (or let session-log.md track it)
2. Work on one or more tasks
3. At session end, record: date, phase, tasks completed, total effort points, actual hours

**After the session:**
- @pm updates PROJECT_PLAN.md velocity table (source of truth)
- Open the velocity tracker artifact in Claude → click "+ log session" → enter the numbers
- Or just tell Claude: "log session: Phase 1, Xola client, 3 pts, 1.25 hrs"

### What the tracker shows you

| Metric | What it means |
|--------|---------------|
| **Logged** | Total points and hours completed |
| **Remaining** | Points left across all phases |
| **Lifetime Velocity** | Your average hrs/pt across all sessions |
| **Recent Velocity** | Your avg over the last 5 sessions (more responsive to current pace) |
| **Projected** | Hours remaining at recent velocity (your realistic forecast) |
| **Phase Progress** | Bar chart showing completion per phase |

### Reading the numbers

- **Lifetime velocity** is your stable baseline. Use it for long-range planning.
- **Recent velocity** catches changes — are you speeding up (learning the codebase) or slowing down (hitting complexity)?
- If recent is significantly higher than lifetime, you might be in a harder phase — normal for polish/integration work.
- If projected hours exceed your available hours before deadline, it's time to cut scope. The tracker doesn't lie.

### Rules of thumb

- Log every session, even short ones (0.25 hrs counts)
- Don't fudge hours to look good — the point is accurate forecasting, not performance review
- Re-estimate tasks that turn out to be bigger than expected — update PROJECT_PLAN.md, note the change
- Velocity stabilizes after ~10 sessions. Before that, take projections with a grain of salt.

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

### Weekly rhythm (when actively building)

**Monday (or first session of the week):**
- Open velocity tracker — check projected hours remaining
- Check PROJECT_PLAN.md — what phase are you in, what's next?
- If starting a new phase, do estimation poker on its tasks

**Each session:**
- Start timer (or note the time)
- Work
- Log session to tracker + PROJECT_PLAN.md

**End of week (or end of phase):**
- Review velocity trend — speeding up or slowing down?
- If behind projection, identify what to cut (PROJECT_PLAN.md has a cuttable tasks list)
- If ahead, resist the urge to add scope. Ship early.

### Cross-project tracking

If running multiple projects (Sailbook V2 + BrewBoat):
- Each project has its own tab in the velocity tracker
- Each project has its own PROJECT_PLAN.md in its repo
- At the end of a session, update whichever project you worked on
- Velocity is per-project — don't mix them. A Supabase CRUD app and an AI agent pipeline have very different hrs/pt profiles.

### The one thing that matters

**Log your sessions honestly.** Everything else — the projections, the poker, the phase progress bars — is downstream of accurate data. Fifteen seconds of logging after each session gives you a forecasting system that actually works.
