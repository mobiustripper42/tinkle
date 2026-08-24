---
name: restart-this
description: Resume after a mid-session pause. Reloads context from the session file and project plan, then presents a focused briefing so work can continue from exactly where it stopped. Does not open a new session.
tools: Read, Edit, Bash, Glob, Grep
---

You are resuming a paused session. Do NOT open a new session entry — this is a continuation of the existing open session.

## Step 0 — Locate the open session

```
grep -l "^status: open" .sessions-worktree/sessions/*.md 2>/dev/null
```

**Exactly one match:** that's `SESSION_FILE`. NEW MODE.

**No match:** check `session-log.md` for `[open]` — LEGACY MODE. If neither, stop and tell the user there's no open session to resume; they probably want `/its-alive`.

**More than one match:** another window has a session open. Report the candidates — `session:`, `branch:`, `started:` — and ask which is yours. Do not sort and do not take the first: `... | head -1` returns the lexically-earliest filename, and session filenames start with a date, so it silently picks the *stale* file whenever that one opened earlier. Nothing errors.

## Step 1 — Stamp the resume time

`RESUME_UTC=$(date -u +%H:%M)`

Append to the session file (in the worktree) immediately after the most recent `[PAUSED ...]` line, **with the `Edit` tool**:

```
**[RESUMED HH:MM UTC]**
```

Not a `sed`/`awk` insertion. `Bash(sed -n *)` is denied fleet-wide and a scripted edit that misses its anchor writes the file back unchanged and exits 0 — so a resume note that silently didn't land looks identical to one that did. `Edit` refuses to write when its target string is missing.

Also capture `BRANCH=$(git branch --show-current)` here — Step 4's briefing prints it.

Commit + push with `git -C` targeting the worktree — **no `cd`**. Shell state doesn't persist between Bash calls, and a stray `cd` that fails leaves the next command running in the wrong tree. `/kill-this` and `/its-dead` were both moved off this pattern after downstream projects hit it; those backports never reached here.

```
git -C .sessions-worktree add sessions/$(basename "$SESSION_FILE")
git -C .sessions-worktree commit -m "Resume note for Session <N>"
git -C .sessions-worktree push origin sessions
```

## Step 2 — Read the pause note

Locate the most recent `[PAUSED HH:MM UTC]` line. Extract:
- Task being worked on
- File / function / step left mid-work
- Immediate next action

## Step 3 — Read project state

Grep `docs/PROJECT_PLAN.md` for the current task context — phase, task ID, acceptance criteria. Do not read the whole file.

## Step 4 — Present resume briefing

```
Resuming Session <N> — paused HH:MM UTC, resumed HH:MM UTC
Branch: <BRANCH>
Session file: <SESSION_FILE> (or "session-log.md" in legacy mode)

Task: [task ID and name]
Left off at: [file/function/step]
Next action: [exactly what to do now]
```

Then say: **"Ready when you are."**

If the pause note is missing or unclear, ask the user where they left off before proceeding.
