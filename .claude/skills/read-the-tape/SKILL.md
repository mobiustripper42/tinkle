---
name: read-the-tape
description: Reviews session JSONL transcripts for workflow anti-patterns and writes one cited observation per session to the seeds observations branch. Changes nothing in this repo. Run `--queue` to drain everything the SessionEnd hook captured; run bare to audit one session. Optional arg: session number, file path, or `--queue`.
tools: Read, Bash, Glob, Grep, Agent
---

You are executing the /read-the-tape skill.

This skill **reads this repo and writes to seeds** (DEC-S039, DEC-S040). It creates no branch here, commits nothing here, opens no PR, and changes no file in this project — not a skill, not `.claude/settings.json`, not a reviewer. Its entire output is one observation file on the `observations` branch in seeds, where `@workout` can see it alongside other repos and other weeks. Steps 0 and 0.5 exist to make that one write possible.

If a finding has a fix that belongs in this project, it still goes in the observation. Someone applies it by hand, later, or doesn't — that is the deliberate trade for an auditor whose output you can trust without reviewing a diff.

## Step 0 — Resolve the seeds checkout

Stop at the first hit:

1. **Skill arg:** if a path to a seeds checkout is passed, use it as `SEEDS`.
2. **Sibling default:** `SEEDS=$(git rev-parse --show-toplevel)/../seeds` if `git -C "$SEEDS" rev-parse --git-dir` succeeds.
3. **Env var:** `$SEEDS_REPO` if set and a git repo exists there.
4. **STOP:** if none resolve, ask: "Where's your seeds checkout? Re-run as `/read-the-tape <transcript> <path-to-seeds>`."

Echo: "Seeds checkout: `$SEEDS`."

Do **not** gate on `seeds-version` and do **not** require `$SEEDS` to be on `main`. Nothing is being synced — this writes one file to an orphan branch that no schema version describes. A project years behind on migrations still observes correctly, and refusing to record evidence because a number is stale would be exactly backwards. Refuse only if the checkout doesn't exist.

**If you're running this skill inside seeds itself**, `$SEEDS` is the current repo. That is fine — seeds audits its own sessions like any other project.

## Step 0.5 — Attach the observations worktree

```bash
git -C "$SEEDS" fetch origin observations
```

- **Worktree already present** (`[ -e "$SEEDS/.observations-worktree/.git" ]` — `-e`, not `-d`: in a
  linked worktree `.git` is a **file**, a `gitdir:` pointer, so `-d` reports missing on a worktree
  that is present): check it's clean
  first — `git -C "$SEEDS/.observations-worktree" status --porcelain`. **If dirty, STOP** and show
  what's there. A dirty observations worktree means an earlier run wrote a file and failed to push
  it, or another session is mid-write; `reset --hard` over either destroys an observation with no
  trace. Only on a clean tree: `git -C "$SEEDS/.observations-worktree" reset --hard origin/observations`
- **Missing, `origin/observations` exists:** `git -C "$SEEDS" worktree add .observations-worktree observations`
- **Missing, no `origin/observations`:** the branch hasn't been bootstrapped. STOP and point at `docs/SPECS/2026-08-workflow-learning-loop.md` § Phase 2. Do not create it from a project session — the branch is seeds' to own, and bootstrapping it from a project is how you get two of them.

Set `SEEDS_OBS="$SEEDS/.observations-worktree"`.

If the fetch or worktree attach fails, **stop before invoking the agent**. Running the audit with nowhere to write means the findings are produced and then discarded, which is the failure DEC-S039 exists to remove.

## Step 0.7 — Pick the mode

**Drain mode** — `/read-the-tape --queue`, or a plain-language "drain the tape queue": work through everything the `SessionEnd` hook has captured since the last drain. This is the normal way to run the skill (DEC-S045). Go to Step 1-Q.

**Single mode** — any other invocation, including bare. Audit exactly one transcript. Go to Step 1. Unchanged from before drain mode existed, and still the right call when you want *this* session looked at now rather than at the next drain.

## Step 1-Q — Drain the queue (drain mode only)

The queue is written by `tape-capture.sh` on session end and lives **outside every repo**, at `$TAPE_QUEUE` or `~/.claude/tape-queue` by default. It holds one copied transcript per session plus `index.jsonl`, one JSON object per line:

```json
{"observed":"2026-08-14","repo":"muster","cwd":"/home/eric/muster","repo_root":"/home/eric/muster","branch":"task/618-x","session_id":"…","transcript":"…","origin":"…","sha":"…","reason":"prompt_input_exit"}
```

**No index, or an empty one: say "tape queue is empty — nothing to drain" and stop.** A clean no-op, not an error. It is also the expected result on a machine where the hook was never installed, so say which of the two it is: if `~/.claude/tape-queue` does not exist at all, the hook is not installed on this box — point at `README.md` § Learning loop rather than reporting an empty queue.

Then, **for each entry, oldest first**:

1. **Check the copy still exists.** If `transcript` is missing from disk, report the entry, drop its index line, and continue — do not stop the drain. A missing copy means someone cleaned the queue by hand; it is not a failure worth abandoning the other entries over.
2. **Derive the slug from `branch`** using the Step 1.5 mapping (`task/644-crew-header` → `644-crew-header`, `main` → `main`). Read Step 1.5 before doing this — the slug is the part after the dev handle, and getting it wrong names the observation badly in a way that is annoying to fix later.
3. **Run Steps 2 and 3** against that transcript, passing `repo` **and `repo_root`** from the entry.

   **The entry's repo is usually not the repo you are standing in** — that is the whole point of a queue. Three things follow, and all have to be done deliberately:

   - **File the observation under the entry's `repo`**, never the current directory's name.
   - **Step 2's prompt tells the agent to read `.claude/skills/` and `.claude/agents/` for context. Those paths resolve against the working directory.** So point the agent at the entry's `repo_root` explicitly — `.claude/skills/` *in `<repo_root>`*. Draining from seeds without doing this hands `@tape-reader` **seeds' own** skills and agents as context for a session that ran somewhere else, and nothing errors: the observation just quietly describes the wrong workflow.
   - **Use `repo_root`, not `cwd`, for those context paths — they are different fields on purpose.** `cwd` is where the session ran; `repo_root` is where its `.claude/` lives. In a linked worktree they differ, and only `repo_root` has anything under it: a session run in `<project>/.sessions-worktree` has a `cwd` holding `sessions/` and no skills or agents at all. That path *exists*, so the missing-directory case below never fires — the agent would simply be handed an empty context directory and would not know.
   - **On an entry with no `repo_root`** (captured before the hook recorded it), fall back to `cwd` and say so in the prompt. Those entries carry the old bug: if `cwd` is a linked worktree, the entry's `repo` is that directory's name too, so file the observation under the parent repo's real name rather than propagating it. **How to tell:** in a linked worktree, `.git` is a *file* containing a `gitdir:` pointer, not a directory — `git -C <cwd> rev-parse --path-format=absolute --git-common-dir` then resolves to the parent repo's `.git`, and its parent directory is the name you want.
   - **If the directory no longer exists** (repo moved or deleted), say so in the prompt and tell the agent to audit the transcript without project context rather than falling back to whatever is at the current path.

4. **Only when Step 3 confirms the observation committed and pushed**, move the copy to `<queue>/drained/` and drop that line from `index.jsonl`. On any other outcome, leave both in place so the next drain retries it.

   **Re-read `index.jsonl` immediately before each removal and filter that fresh copy** — do not read the whole index once at the start of the drain and write back a filtered version at the end. A drain runs an LLM and a git push per entry, so it can span minutes, and the capture hook appends to this same file with no lock. A batch rewrite silently discards any session that ended mid-drain: the index line vanishes while its transcript copy sits in the queue, unindexed and invisible to every future drain. Filter by `session_id`, write to a temp file, move it into place.

Report at the end: how many drained, how many retried, how many dropped for a missing copy.

**Also report orphaned copies — transcripts in the queue with no index line naming them.** `ls` the queue and check each `.jsonl` other than `index.jsonl` against the index. An orphan is invisible to every drain, forever, and nothing else looks for it. They should no longer be created — the hook writes to the path the index already names — but any left from before that fix are still sitting there, and one of them was a *more complete* copy of a session whose indexed transcript had been truncated by an earlier fire. Do not delete them: report the filenames and let the operator decide, since an orphan is usually the better copy rather than a stray.

**Do not batch the observations into one file.** One session, one observation, same as single mode — `@workout` counts recurrences across observations, and merging five sessions into one file makes five sightings look like one.

**Expect boring sessions.** Every session is captured now, not just the ones someone thought were interesting, so most entries will produce a clean-run observation. That is working as intended: a clean run is the evidence that retires a rule. Keep them to one line, as the spec already requires.

## Step 1 — Find the transcript

**If a session file path is given as the arg** (e.g. `/read-the-tape sessions/2026-05-03-0339-eric-pm-rework.md`):
Read its YAML frontmatter and pull `transcript:`. Use that path directly. If the field is empty or missing, fall back to the heuristic below.

**If a JSONL file path is given as the arg** (e.g. `/read-the-tape ~/.claude/projects/foo/abc123.jsonl`):
Use it directly.

**No arg — heuristic:**

Compute the project's JSONL directory path via Bash:

```
echo "$HOME/.claude/projects/$(pwd | tr '/' '-')"
```

Capture stdout as `JSONL_DIR`. Then use the **Glob** tool to list the JSONLs:
- `path: <JSONL_DIR>`
- `pattern: *.jsonl`

Glob returns absolute paths sorted by modification time, newest first. No basename re-prefixing needed — the result is already absolute.

Default to the **second-newest** JSONL (`result[1]`) — the current session's JSONL is always the newest (being written live); the one to audit is the previous one. If only one JSONL exists, use `result[0]`.

The Glob tool is used in place of `ls *.jsonl` because the Bash form trips two harness validator rules (tree-sitter-bash on `"$VAR"/*.glob`, and a newer rule on `cd "$VAR" && ls 2>/dev/null`). See its-alive Step 5 for the full note.

### Step 1.5 — Derive the slug (just the slug)

The slug names the observation file, and getting it wrong is easy in a specific way. A session file is `YYYY-MM-DD-HHMM-<dev>-<slug>.md`, so **the slug is only the part after the dev handle** — everything before it is date, time, and dev.

```
sessions/2026-08-05-0842-eric-main.md              → slug is  main
sessions/2026-08-04-1130-eric-time-clock.md        → slug is  time-clock
sessions/2026-08-01-0915-eric-644-crew-header.md   → slug is  644-crew-header
```

**Do not pass the whole filename or its stem.** The observation is named `<observed-date>-<repo>-<slug>.md`, so a full stem produces `2026-08-06-muster-2026-08-05-0842-eric-main.md` — the date twice, the dev handle for no reason, and a name that sorts by the *audited* session's date inside a directory that sorts by the *observation* date. Observed on the first live run; this step exists because of it.

If no session file was passed, derive the slug from the branch the audited session ran on (`task/644-crew-header` → `644-crew-header`, `main` → `main`), the same mapping `/its-alive` Step 3 uses. If neither resolves, ask — don't invent one.

## Step 2 — Invoke @tape-reader

Pass the transcript, the observations worktree, and the slug:

> "Analyze the session transcript at `<path>`. Repo: `<project dir name>`. Session slug: `<slug>`.
> Observations worktree: `$SEEDS_OBS` — write this run's observation there and push to the `observations` branch.
> Project skills are in `.claude/skills/`, agents in `.claude/agents/` — read them for context.
> Change nothing in this repo. The observation is your only output."

The agent analyses, presents its findings, and writes the observation. `Edit` is withheld from it, which removes the habitual path — but it keeps `Write` and `Bash`, so Step 3 is the check that actually catches a violation.

## Step 3 — Confirm the observation landed, and that nothing else did

After the agent returns, check both ends:

```bash
git -C "$SEEDS_OBS" log --oneline -1     # expect this run's observation, pushed
git -C "$SEEDS_OBS" status --porcelain   # expect clean
git status --porcelain                   # expect UNCHANGED from before the run
```

- **The observation landed.** If the file is uncommitted or unpushed, say so plainly and surface the path — one stranded in a worktree nobody looks at is indistinguishable from one that was never written, and the next `reset --hard` in Step 0.5 discards it silently.
- **This repo is untouched.** If `git status` shows anything the agent introduced, that is a defect in the agent, not a change to review: report it and revert it. The whole value of an observer is that its output needs no diff review (DEC-S040).
