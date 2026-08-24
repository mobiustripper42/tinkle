---
name: kill-this
description: Per-task PR + session-log update. Build check, commit code, push the task branch, run code review, open a PR, and append a `## Task <N>` block to the running session file on the orphan `sessions` branch. May run multiple times in one Claude window — one per task. Pair with `/its-dead` once at the end of the window. Time math + version bump moved to `/retro` (DEC-S013).
tools: Read, Edit, Write, Bash, Glob, Grep, Agent
---

You are shipping one task. Under DEC-S013, `/kill-this` runs **per task**, not per session — there may be N invocations between `/its-alive` and `/its-dead`. Each one opens its own PR and appends one `## Task <N>` block to the session file (which lives on the orphan `sessions` branch via `.sessions-worktree/`, per DEC-S014).

## Step 0 — Locate the session file, capture the branch

```
grep -l "^status: open" .sessions-worktree/sessions/*.md 2>/dev/null
SESSION_FILE=<the one match>
BRANCH=$(git branch --show-current)
```

**No match:** STOP. The user must run `/its-alive` first.

**More than one match:** another window has a session open. Report the candidates — `session:`, `branch:`, `started:` — and ask which is yours. Do not sort and do not take the first: `... | head -1` returns the lexically-earliest filename, and session filenames start with a date, so it silently picks the *stale* file whenever that one opened earlier. Nothing errors.

`BRANCH` is read from the current directory, and under DEC-S048 that is correct by construction: a session starts in the checkout its work lives in and stays there. If that stops being true, fix the session, not this skill — every wrong-tree symptom downstream is that one broken assumption wearing a different hat.

Read the file's frontmatter to get session number `N` and the current `pr_numbers:` list.

Determine the next task index:
```
TASK_NUM=$(($(grep -c "^## Task " "$SESSION_FILE") + 1))
```

## Step 1 — Build check

Look up the project's build check in `.claude/CLAUDE-context.md §Commands` (e.g. `npm run build`, `cargo build`, `make`), **with the Read tool** — never a `sed`/`grep` one-liner. Run it. Fix errors before proceeding. Do not commit broken code.

If no build step is defined (markdown-only / domain project), skip silently.

## Step 2 — Commit code on the task branch

Stage all uncommitted code changes on the **task branch** (the current `$BRANCH` — NOT the sessions worktree). The session-file update happens later in Step 5 and goes to the sessions branch, not here.

```
git add -A
git commit -m "<phase/task summary>

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

**Name the files being staged**, one line from `git diff --cached --name-only`. `git add -A` sweeps whatever is in the tree, so a file list that doesn't look like the task you just did is the signal — an unrelated edit riding along is caught here or not at all.

If there is nothing to commit, surface that and stop here — no PR for no code.

**Push the branch — do not open a PR yet:**

- **On `main` (DEC-S005 solo flow, unprotected main):** `git push origin main`. Skip Steps 3+4 — no PR. Go to Step 5.
- **On a `task/*`, `claude/<slug>`, or feature branch:** `git push -u origin $BRANCH`. Continue.

Capture `SUBJECT=$(git log -1 --format=%s)` for the PR title.

## Step 3 — Code review

Run `@code-review` against `git diff HEAD~1`. Capture the findings — needed for the PR body and the task block.

When addressing review findings before opening the PR: Read every file before editing it (parallel writes fail silently without a prior Read).

### Step 3.5 — High-blast-radius check (does this PR want `/code-review ultra`?)

`@code-review` hunts the project's known invariants. `/code-review ultra` is a different tool — it launches multiple agents to audit the branch independently from different angles and filters by confidence. It is **user-triggered and billed; Claude cannot launch it.** Do not attempt to run it via Bash or otherwise.

Get the project's trigger table from `.claude/CLAUDE-context.md` under `## Blast-Radius Triggers` **with the Read tool** — never a `sed`/`grep` one-liner. Then match the table against the branch diff (`git diff $(git merge-base HEAD main)...HEAD --name-only`). If that section is absent, fall back to the four generic triggers below.

| Trigger | What to match |
|---|---|
| **Money moving** | payment-provider calls, webhook handlers, refunds, fee/tip/balance math |
| **Money being computed** | hours, rates, pay periods, tips, invoice totals — and any export that carries them |
| **Auth / capability URL** | session issue and validation, token minting, bearer or signed-link paths, permission checks |
| **Data-changing migration** | a new migration containing `drop`, `alter … type`, `update`, or `delete` (an additive `add column` does **not** trigger) |
| **Too big to review well** | the diff is large or sprawling enough that you would not confidently sign off on it yourself |

**The test, when a path isn't listed:** *does a number this code produces end up on someone's paycheck or invoice?* If yes, it's the money path — whether or not a payment provider is anywhere near it. The first two rows exist separately because defining "money" by **where money moves** misses where it is **computed**: a time-clock table can land with no payment file in the diff, and a wrong timestamp, a mis-bucketed pay period, or a double-counted punch is a wrong payment that no provider-shaped trigger would ever catch.

**If one or more hit, run the free local pass first, then surface the paid one.** A trigger that only ever produces a suggestion to spend money produces nothing on the days you decide not to spend it — and those are exactly the PRs it fired on.

1. **Run `/security-review`** against the branch. It is local, unbilled, and aimed at this class: authorization boundaries, injection, secret handling, unsafe defaults, failure modes that fail open. This is not a duplicate of Step 3 — `@code-review` hunts the project's conventions and invariants; this hunts the ways a hostile or malformed input gets through. Fold its findings into the PR body under their own heading, so the reviewer can see which pass produced what.


2. **Then print exactly this and continue** — never block, never run the billed tool:

```
⚠ This PR touches: <triggers>.
  Ran /security-review (local, free) — findings above.
  `/code-review ultra` is the deeper multi-agent pass: yours to run, I can't.
```

**Where each one earns its cost:** `/security-review` reads the diff once, carefully. `/code-review ultra` fans out across several independent agents and filters by confidence, which is what catches the finding a single careful read talks itself out of. Run the local pass always on a trigger; save the billed one for a genuinely novel money or auth path, where being wrong is expensive and one reviewer's confidence is not enough.

If none hit, run nothing extra. Docs, seeds, agent/skill files, dev tooling, and single-surface UI never trigger it — their blast radius stops at the dev environment.

### Step 3.6 — Say what actually ran

**Print this every time, including when nothing triggered.** Not as a summary of findings — as a receipt of which passes happened.

```
Review passes:
  ✓ @code-review       — <N> findings: <one-line verdict>
  ✓ /security-review   — <N> findings: <one-line verdict>      ← only when a trigger hit
  ⊘ /security-review   — not run (no blast-radius trigger)     ← otherwise
  ⊘ /code-review ultra — never automatic; yours to invoke
```

**Why this is its own step.** With three possible passes, "no news" is ambiguous in the one direction that matters: a review that silently didn't run looks exactly like a review that ran clean. That ambiguity was already reported on the two-pass version — a `⚠ consider ultra` line appeared and the operator could not tell from the output whether `@code-review` had run at all. Adding a third pass makes it worse unless the receipt is unconditional.

**A pass that errored is `✗`, not a missing line.** If `@code-review` fails to return, or `/security-review` can't run, say so on its row and continue to the PR — but never let a failed pass render as a quiet absence. The whole point of the receipt is that absence is never something the reader has to infer.

**Why this is a step and not a rule to remember:** the trigger is a property of the diff, and the moment you'd need to recall it is the moment you're least likely to (late, task finished, PR ready). Checking the diff is reliable; remembering is not.

## Step 4 — Open the PR

Resolve base branch — always the project's active trunk (DEC-S022):
```
BASE=main
```
`main` is the active trunk in every project. A `production` branch, if the project has one, is a downstream deploy pointer advanced by `/promote-production` — it is **never** a PR base. (If a project's default branch isn't `main`, set `BASE` to that; the steady state is `main`.)

### Step 4.0 — Resolve existing PR state for this branch

Set `EXISTING_PR_STATE` to one of `OPEN`, `MERGED`, `CLOSED`, `NONE`. Method 1 = `gh pr view "$BRANCH" --json url,state 2>/dev/null`. Method 2 = `mcp__github__list_pull_requests` (head: `<owner>:$BRANCH`, state: all). Method 3 = STOP and ask the user.

- **OPEN**: capture `PR_URL` and `PR_NUMBER`, skip Step 4.2 (no duplicate). Note in the task block.
- **MERGED / CLOSED**: unusual — this branch was already shipped. Ask the user: "Existing PR is `$EXISTING_PR_STATE`. Open a new PR on top? (y/n)" — if no, surface and stop; if yes, proceed to Step 4.2.
- **NONE**: proceed to Step 4.2.

### Step 4.2 — Create the PR

Compose `BODY`:

**## Summary**
One-line description.

**## Files changed**
Bulleted list from `git diff --name-only $BASE..HEAD`.

**## Code review**
Lead with the Step 3.6 receipt — which passes ran, which didn't, and why — then the findings from each, under its own sub-heading so the reviewer can tell them apart. "Clean bill of health" is a statement about a pass that *ran*; never write it in place of a pass that didn't.

**## Test plan**

Generated by you from `git diff --name-only $BASE..HEAD`. **Always two sections, in this order and under these headings**, because they answer different questions and one cannot substitute for the other:

**`### Verified (automated)`** — what you ran and what it returned. Commands and counts: the gate, the specific spec files, the new cases and what each one pins. A number that isn't in the output is a number you made up.

**`### Verify by hand`** — what *the reviewer* must do, because a machine did not and could not check it.

Each hand step is three things, and a step missing any of them is not a step:

1. **Starting state** — the route, the signed-in role, and any seed or setup command. "Open the app" is not a starting state.
2. **The exact action** — the control by its visible label, and the viewport if it matters. "Check the drawer works" is not an action; "at 375px, tap the ☰ button in the header" is.
3. **What you should see** — stated so that *not* seeing it is unambiguous. Where the change fixes a reported symptom, say what it did **before**, so the reviewer can tell a fix from a coincidence.

Close with **`Reset:`** — what to undo afterwards, or `none`. A reviewer who won't touch prod data because they can't tell what's reversible has been given no test plan at all.

**Open the hand section with `#### Setup`, and make it literal.** Everything below is drawn from a test plan that worked — one the operator had to ask for, which is why it is written down here instead of depending on someone thinking of it.

- **The commands, in order, in one block** the reviewer can paste. Not "seed the database" — the actual command names.
- **Flag anything destructive on the line itself.** `npm run db:reset:dev  # destructive: wipes your dev data` is the difference between a reviewer running your plan and closing the tab.
- **Name the non-obvious prerequisite, with why it's needed.** The step someone will skip because nothing suggests it matters — *"`db:seed:crew` is not optional: migration 0018 seeded a provisional admin roster and 0019 deletes it, so a freshly-migrated dev DB has zero admins and every `/admin/*` route renders the signed-out screen."* That sentence is worth more than the rest of the setup block combined, because it is the one nobody can derive.
- **Spell out how to sign in** — the exact URL, the exact button label, and which identity it mints. "Sign in as an admin" is not a step if getting an admin is the hard part.
- **Say what is *not* needed.** *"No Stripe, no `stripe listen`, no webhook — this touches no money path."* A reviewer who doesn't know whether to start the payment stack will either waste ten minutes or skip the whole plan.
- **Use literal values, and caveat the generated ones.** Write the real dates and IDs. If the seed builds them relative to today, say so and give the anchor: *"dates assume you seed on 2026-08-07 → the window is 2026-09-10 … 2026-09-16; if you seed on a different day, shift the month and keep the day-of-month."*
- **Carry forward a gotcha that bit last time** if one applies — a leftover env var, a stale process, a cached build.

**Test the abort path, not just the happy one.** Where the change adds a confirm, a cancel, or a destructive action, a step that clicks Cancel and asserts **nothing was written** is worth more than the one that clicks OK — it is the path nobody writes a test for and the one that silently does damage when it's wrong.

**A green suite never satisfies the hand section on a rendered change.** This is the rule the others exist to serve, and it is written from a specific failure: a PR shipped with five numbered test-plan items — full gate green, eight new e2e cases, an entire 116-test mobile project at 375px, nine specs rerouted — and **not one step a human performed**. It read as thorough. Four defects reached the operator within minutes of merge: a control behind a modal backdrop, an undersized touch target, a dead-end link, and a drawer that could not be closed at 375px. Every suite was green the whole time. **None of those four is a class of defect a passing test can catch**, because each is a question about what a person can reach, hit, read, or escape.

So when the diff touches anything rendered, the hand section answers, in whatever form fits:

- **Can you reach it?** Not "is it in the DOM" — is it reachable by the path a real user takes.
- **Can you operate it at 375px?** Tap targets, overlap, anything behind a backdrop or off-screen.
- **Can you get back out?** Close, cancel, escape, back. A surface with no exit is the defect that testing-by-assertion misses most reliably.
- **Does the thing you replaced still work?** Whatever the change routed around, moved, or renamed.

**If there is genuinely nothing to check by hand, write one line saying why** — `Docs only, no rendered surface`, or `Script change; behaviour covered by the new negative control`. An absent section is indistinguishable from a forgotten one, which is the same ambiguity Step 3.6 exists to remove.

**Non-UI changes still get both sections.** A migration's hand step is applying it and confirming the expected shape and row counts, plus what happens to existing rows. A money-path change is reconciling an amount end to end. A capability-URL change is confirming a stale or forged token is refused.

**Every number in the body says which kind it is** — `issue #699`, `PR #707`, never a bare `#699`. Issues and PRs come from one shared GitHub counter, so they interleave and nothing in the number distinguishes them. **The single exception is the `closes #<issue>` line**, which is GitHub syntax and stops auto-closing if you prefix it. Write that one bare and say the kind in the prose around it.

Try in order:
1. `gh pr create --base "$BASE" --head "$BRANCH" --title "$SUBJECT" --body "$BODY"`
2. MCP `mcp__github__create_pull_request` fallback.
3. STOP: print body for the user to paste manually; note "PR not opened" in the task block.

Capture `PR_NUMBER` and `PR_URL`.

## Step 5 — Append the task block to the session file (sessions branch)

The session file lives on the orphan `sessions` branch at `.sessions-worktree/sessions/<file>.md`. Read it first.

Compose the task block:

```
## Task <TASK_NUM>: <one-line title>

**Completed:**
- <bullet list of what got done, with file paths>

**Code review:** <findings summary or "Clean">
**PR:** [PR #<PR_NUMBER>](<PR_URL>)
**Points:** <effort estimate>
**Blocked:** <only if blocked>
**Branch:** <BRANCH>
**Opened at:** <ISO 8601 timestamp>
```

Use the **Edit** tool on `$SESSION_FILE` (the worktree path) to:

1. Append the `## Task <TASK_NUM>:` block before the `**Next Steps:**` section near the bottom.
2. Update the frontmatter `pr_numbers:` list to append `<PR_NUMBER>`. Example: `pr_numbers: [42, 43]`.

Then commit + push using `git -C` to target the worktree directory (no `cd` — shell state doesn't persist between Bash calls, and a stray `cd` that fails leaves the next command running in the wrong tree):

```
git -C .sessions-worktree add sessions/$(basename "$SESSION_FILE")
git -C .sessions-worktree commit -m "Session <N> — log Task <TASK_NUM> (PR #<PR_NUMBER>)"
git -C .sessions-worktree push origin sessions
git -C .sessions-worktree checkout sessions 2>/dev/null || true
```

The final `checkout sessions` re-pins the worktree HEAD to the `sessions` branch — guards against a detached-HEAD state if anything upstream rewrote history.

The user's main checkout never moves; the task branch stays clean (no session-file pollution).

## Step 6 — Surface to the user

```
Task <TASK_NUM> shipped.
PR: <PR_URL>
Code review: <one-line summary>

Next: keep working in this session (cut another branch + `/kill-this` again), or run `/its-dead` to close the session.
```

If `EXISTING_PR_STATE` was `OPEN` and Step 4.2 was skipped, surface the existing PR URL and note that the task block now references the pre-existing PR.

## Notes

- **No time math, no version bump, no CHANGELOG.** All deferred to `/retro` per DEC-S013. This skill ships a task and logs it; that's it.
- **Branch ownership.** Code commits go to the current task branch. Session-file commits go to the sessions branch via the worktree. Two completely separate timelines.
- **Multiple PRs per session is normal.** Each `/kill-this` appends a `## Task <N>` block; the `pr_numbers:` list grows. `/retro` reads this list to enumerate the PRs to query for merge timestamps.
- **Merge ordering is free.** The user can merge each PR whenever — before the next `/kill-this`, after `/its-dead`, days later. Retro reads GitHub at retro time and gets the merge timestamps regardless.
- **Atomicity at `/its-dead`.** Once `/its-dead` writes `status: closed`, the session file is never modified again. `/retro` reads it but only writes to `RETROSPECTIVES.md` and (on dev projects) to `package.json` / `CHANGELOG.md` / git tags.
