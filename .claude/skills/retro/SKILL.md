---
name: retro
description: Phase-end retrospective. Closes the current phase. Under DEC-S013, retro is also where per-session time math runs — for every session in the phase window, it computes wall_clock and active time (active = wall_clock - breaks, breaks inferred from the transcript JSONL) from `started`, `ended`. Aggregates to one phase velocity: active h/pt. Marks PROJECT_PLAN.md `[x]`, reconciles drift, writes RETROSPECTIVES.md, runs version bumps (patch per merged PR + minor at phase close on dev projects), prompts retro notes. Optionally chains into `/start-phase`.
tools: Read, Edit, Write, Bash, Glob, Grep, Agent
---

You are running the phase-end retrospective. Work for this phase is complete (or you've decided to call it done and move scope).

Under DEC-S013, this skill **owns all per-session time math** that used to live in `/its-dead` and **all version bumps** that used to live in `/its-dead` (patch per merge) and `/retro` (minor at close). Session files are atomic event logs; retro is where they get turned into numbers.

## Step 0 — Identify the current phase

Find phase N as the **lowest phase number with any open issues** OR (if all closed) the highest phase with `[ ]` rows in PROJECT_PLAN.md but no `[x]` marks yet.

```
for phase in 0 1 2 3 4 5 6 7 8 9; do
  open=$(gh issue list --label "phase:$phase" --state open --json number -q 'length')
  if [ "$open" -gt 0 ]; then echo "Phase $phase has $open open issues"; break; fi
done
```

Confirm: "Run retro for Phase **N**?" Wait.

## Step 1 — Account for all phase issues

```
gh issue list --label "phase:<N>" --state all --json number,title,state,labels --limit 100
```

For each open issue, ask the user: "Move to next phase, leave open, or close as won't-do?"
- **Move:** swap `phase:N` → `phase:N+1`.
- **Leave open:** record in retro.
- **Close as won't-do:** `gh issue close <N> --reason "not planned" --comment "Closed at Phase N retro — descoped."`

## Step 2 — Per-session time math (DEC-S013 — moved from `/its-dead`)

Find every session file in the phase window. Phase window = first issue's `createdAt` → last issue's `closedAt` (use `gh issue list --label "phase:<N>" --state all --json createdAt,closedAt`).

For each session file in `.sessions-worktree/sessions/` whose `started:` falls within the phase window (or `started:` < phase_start AND `ended:` is within — span sessions count too):

### Step 2.1 — Read frontmatter

Parse `started`, `ended`, `pr_numbers`, `points`, `transcript` from the session file's YAML frontmatter. Required: `started`, `ended`. If `ended` is empty (session was abandoned or unclosed), skip with a note in the retro.

### Step 2.2 — wall_clock

`wall_clock = (ended − started) in hours, rounded to nearest 0.083h`. Always defined.

### Step 2.3 — Break inference from transcript

If `transcript` is set and the file is readable:
1. Read the JSONL line by line; parse each line's `timestamp` field.
2. Sort timestamps ascending.
3. Walk pairwise. Any gap > 15 min counts as idle.
4. Sum idle minutes within `[started, ended]` to get `total_breaks_hours`.

If the transcript is unreadable or missing, set `total_breaks_hours = 0` and note `inference: transcript-unavailable` in the per-session line of the retro.

### Step 2.4 — active_time (the headline number)

```
active_time = max(0, wall_clock - total_breaks_hours)
```

Active time is wall-clock minus inferred idle — the time actually spent at the keyboard on this session's work. Round to nearest 0.083 h (5 min). This is the **single** velocity input. No PR timestamps are fetched; no time is split into "dev" vs "review."

**Why there is no dev/review split anymore (retiring the DEC-S015 per-PR window model).** Through Phase 7 the retro also computed `dev_time` and `review_time` via per-PR windows. That math is retired. It assumed each PR was merged before the next opened, but the actual workflow opens PRs in a burst and merges them whenever ("merge PRs whenever — order doesn't matter" — CLAUDE.md). Under that real pattern the per-PR anchors collapse: `dev_time` falls to a fictional ~0.03–0.09 h/pt and the overlapping review windows sum past wall-clock (a real case: Session 25 reported 26.8h of "review" inside a 9.2h session — physically impossible). Every retro since Phase 3 footnoted the split as untrustworthy and forecast on active anyway. So we keep the number that was always the headline and drop the two that were always disclaimed. There is no cheap fix for the split — correctly attributing per-task time would require reconstructing keystroke spans from the transcript, which is exactly what `active = wall - breaks` already approximates.

### Step 2.5 — Per-session line for the retro

Build one row per session for the RETROSPECTIVES.md table:

```
| Session N | YYYY-MM-DD | wall_clock | breaks | active | points | PRs |
```

Hold these numbers — they get summed in Step 3. (`PRs` is just the `pr_numbers` list from frontmatter, for reference — no timestamps.)

## Step 3 — Phase metrics

Sum the per-session numbers:
- `phase_wall_clock` = Σ wall_clock
- `phase_breaks` = Σ breaks
- `phase_active` = Σ active   (= `phase_wall_clock - phase_breaks`)
- `phase_points` = Σ points (also confirm against `points:N` label sum from closed issues — flag mismatch)
- `phase_sessions` = count

One velocity:
- `active / point` — active hours per effort point. **This is the headline and the only number to forecast on.** `wall_clock` is kept as raw bookkeeping (it carries overnight gaps and idle), but `wall_clock / point` is not a velocity — don't quote it as one.

## Step 4 — Update PROJECT_PLAN.md

Mark all closed phase tasks `[x]`. For each row:
```
| 1.1 | Task description | 3 | [x] [#42](url) |
```

Reconcile drift: issues with `phase:<N>` labels that don't appear in PROJECT_PLAN.md (added mid-phase). Add rows with status `[x] [#N](url)` and inline note `Added during P<N> retro`.

Update the velocity table at the top:
```
| Phase | Sessions | Points | Wall (h) | Breaks (h) | Active (h) | h/pt (active) |
|-------|----------|--------|----------|------------|------------|---------------|
| N     | <count>  | <pts>  | <wall>   | <breaks>   | <active>   | <active_pt>   |
```

Append one row per phase as they complete.

## Step 5 — Prompt retro notes

Ask three questions, one at a time, capture verbatim:
1. **What worked?**
2. **What didn't?**
3. **What changes for next phase?**

## Step 5.5 — PM retro commentary

Invoke `@pm` (Sonnet) with the full retro context: phase number + name, metrics from Step 3, user's verbatim answers, the per-session table from Step 2.6, closed-issue list with descoped/moved notes, and `docs/RETROSPECTIVES.md` for cross-phase comparison. Let `@pm` read the session files themselves for in-the-trenches detail.

`@pm` returns 3–5 short paragraphs on pace, scope, patterns, a reaction to the user's answers (not a paraphrase), and a forward-looking note.

Show the commentary verbatim:
> **PM read on Phase N:**
>
> <commentary>
>
> Use (a), edit (e), or skip (s)?

- **Use:** carry forward.
- **Edit:** ask "What would you change?" — apply edits, carry forward.
- **Skip:** omit the section from RETROSPECTIVES.md.

## Step 6 — Append to RETROSPECTIVES.md

Read `docs/RETROSPECTIVES.md` first (Edit requires a prior Read). If it doesn't exist, create it with Write and the header `# Retrospectives\n\n`. Otherwise Edit the file by replacing the `# Retrospectives\n\n` header with `# Retrospectives\n\n## Phase <N> — <YYYY-MM-DD>\n\n...full block...\n\n` so the new phase lands at the top. Block template:

```
## Phase <N> — <YYYY-MM-DD>

**Sessions:** <count>
**Points:** <points completed> / <planned> (<%>)
**Wall clock:** <wall>h  (raw elapsed — includes overnight + idle)
**Breaks:** <breaks>h
**Active time (wall - breaks):** <active>h ← honest headline
**Velocity:** <active/pt> h/pt active  ← the only forecast number
**Issues:** <created> created, <closed> closed, <moved> moved to Phase <N+1>

### Per-session breakdown
| Session | Date | Wall | Breaks | Active | Points | PRs |
|---------|------|------|--------|--------|--------|-----|
| <row>   | ...  | ...  | ...    | ...    | ...    | ... |

### What worked
- <verbatim>

### What didn't
- <verbatim>

### Changes for next phase
- <verbatim>

### Scope changes
- [Tasks added mid-phase, moved out, descoped]

### PM read
<commentary from Step 5.5, verbatim or edited — omit section if skipped>
```

## Step 7 — Commit (sessions branch updates are read-only here)

Session files were already finalized by `/its-dead` and are not modified by this skill — DEC-S013 atomicity.

```
git add docs/PROJECT_PLAN.md docs/RETROSPECTIVES.md
git commit -m "Phase <N> retro — <points> pts in <active>h active (<active/pt> h/pt)"
git push origin <BRANCH>
```

## Step 8 — Version bumps (dev projects only — DEC-S013 moved patch bumps from `/its-dead` here)

Run only if `package.json` exists at the repo root (dev-project signal — DEC-S007).

Resolve working branch — always the active trunk (DEC-S022):
```
WORKING_BRANCH=main
```
Bumps and tags land on `main` directly; `production` (if any) only moves at `/promote-production`.

If `BRANCH != $WORKING_BRANCH`: STOP. Tell the user "Switch to `$WORKING_BRANCH` and re-run /retro." Wait.

### Step 8.1 — Enumerate merged PRs in the phase window

```
gh pr list --state merged --search "merged:>=<phase_start_iso> merged:<=<phase_end_iso>" --json number,title,mergedAt --limit 100
```

Sort by `mergedAt` ascending. Each PR earns one patch bump + one CHANGELOG entry.

### Step 8.2 — Patch-bump per PR

For each PR in order, sequentially:

a. **Bump patch:** `NEW_VERSION=$(npm version patch --no-git-tag-version | tr -d 'v')`.

b. **CHANGELOG entry.** If `CHANGELOG.md` doesn't exist, create with `# Changelog\n\n`. Read it first; if it doesn't start with the literal `# Changelog\n` header, STOP and surface (don't guess where to insert). Prepend after the header:
   ```
   ## [<NEW_VERSION>] - <YYYY-MM-DD>
   - PR #<N>: <title>
   ```

c. **Commit + tag (main only):**
   ```
   git add package.json CHANGELOG.md
   [ -f package-lock.json ] && git add package-lock.json
   git commit -m "Bump version to v<NEW_VERSION> (PR #<N>)"
   git tag "v<NEW_VERSION>"
   ```
   Tags land on the trunk at bump time. Promotion to `production` (if the project has it) carries the already-tagged commit — `/promote-production` does not tag.

### Step 8.3 — Minor-bump at phase close

After all PR patches:

a. `NEW_VERSION=$(npm version minor --no-git-tag-version | tr -d 'v')` — zeros the patch (e.g. 1.2.7 → 1.3.0).

b. CHANGELOG entry:
   ```
   ## [<NEW_VERSION>] - <YYYY-MM-DD> — Phase <N>
   - <points> pts shipped across <session count> sessions (<active/pt> h/pt active)
   - See `docs/RETROSPECTIVES.md` for the full retro
   ```

c. Commit + tag (main only):
   ```
   git add package.json CHANGELOG.md
   [ -f package-lock.json ] && git add package-lock.json
   git commit -m "Phase <N> close — bump to v<NEW_VERSION>"
   git tag "v<NEW_VERSION>"
   ```

### Step 8.4 — Push

```
git push origin "$WORKING_BRANCH"
```
If any tags were created in 8.2 or 8.3: `git push origin --tags`.

Echo: `Phase <N> closed at v<NEW_VERSION>` (and `tagged` if main).

## Step 9 — Offer next phase

"Phase <N+1> is next. Run `/start-phase <N+1>` now or stop here?" Let the user invoke `/start-phase` themselves — don't auto-chain.

## Step 10 — Summary

```
Phase <N> closed.
Points: <P> in <active>h active time (<active/pt> h/pt)
Wall: <wall>h | Breaks: <breaks>h | Sessions: <count>
Issues: <closed>/<created> closed; <moved> moved to Phase <N+1>
Retro: docs/RETROSPECTIVES.md
Version: v<NEW_VERSION>  (dev projects only; skipped if no package.json)
```

## Notes

- **Session files are read-only here.** Retro reads them; never writes. DEC-S013 atomicity.
- **GitHub queries can fail.** Time math no longer touches GitHub at all — active time comes from the session file + transcript. `gh` is only used for issue accounting (Steps 1 and 3's points cross-check) and version bumps (Step 8). If it's down, skip those, note it, and let the user rerun. Don't guess.
- **The headline velocity is `active / point`, and it's the only one.** `active = wall_clock - breaks`. Wall-clock velocity is inflated by overnight gaps and idle — don't quote `wall / point` as a velocity. There is no `dev_time` or `review_time` (retired — see Step 2.4).
- **Old retros carry dead columns.** Phase retros written under the per-PR model have `Dev` / `Review` columns and a dev/review velocity, all artifacts — the `Active` figure in those same retros was always the real headline. Phases that predate active time have only a legacy `Velocity: X hrs/pt` and no active number; **don't blend those with active h/pt** — they're a different, older metric (the standalone velocity extractor handles this split for you).
