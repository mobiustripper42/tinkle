# [Project Name] — Claude Code Project Context

> **Read `.claude/CLAUDE-context.md` first.** It holds this project's name, stack, data model, commands, and any project-specific overrides to the workflow and conventions below. Treat it as authoritative for every project-specific fact. If the file does not exist, stop and tell the user to create it from the seeds template (`dev/claude/CLAUDE-context.md`) before continuing.
>
> This `CLAUDE.md` is a **seeds-managed shell** (DEC-S019): it carries only project-agnostic workflow guidance and syncs from seeds untouched. Do **not** add project-specific content here — it belongs in `.claude/CLAUDE-context.md`, or the next sync will overwrite it.

## Key Docs
| File | Purpose |
|------|-------|
| `docs/SPEC.md` | What we're building — scope, V1 vs V2 vs V3 |
| `docs/decisions/` | Why we made each architectural choice — **one decision, one file**, `DEC-<id>-<slug>.md` (DEC-S036) |
| `docs/DECISIONS.md` | **Generated** topic index over `docs/decisions/`. Never edit it by hand |
| `docs/PROJECT_PLAN.md` | Phases, scope, velocity. **Phase-boundary doc** — read at planning, written at retro. Current-phase tasks live in GitHub Issues. |
| `docs/RETROSPECTIVES.md` | Phase-end retrospectives — written by `/retro` |
| `docs/AGENTS.md` | Agent and skill specs (canonical). |
| `docs/VELOCITY_AND_POKER_GUIDE.md` | Estimation methodology |
| `docs/CHEATSHEET.md` | One-page printable skill reference |
| `sessions/*.md` (on orphan `sessions` branch via `.sessions-worktree/`) | Per-session files — `YYYY-MM-DD-HHMM-<dev>-<slug>.md`. Atomic after `/its-dead` closes (DEC-S013); orphan branch decouples session log from any code branch (DEC-S014). |
| `.claude/seeds-version` | Schema version this project was last installed at. Nothing reads it automatically (DEC-S040) — compare it against seeds' `seeds-version` by hand to see which migrations this project owes. |
| `.claude/project-type` | Project type — `webapp` or `tool`. Says which template files this project has no use for (DEC-S011). Optional. |

Project-specific docs are listed in `.claude/CLAUDE-context.md` under `## Additional Docs` — including BRAND.md, USER_STORIES.md and DEV_REFERENCE.md, which are webapp-shaped and legitimately absent from a CLI, docs or firmware project. The shell lists only docs every project has; a shell that names a doc a whole project type doesn't need is a dead reference in every one of them.

## Micro Workflow (every task, no exceptions)

1. **Spec it** — poker estimate + acceptance criteria. Before writing code, pin what "done" looks like: enumerate the concrete set from source and confirm it with me. My live words override prior docs. **Get the whole spec down before step 4** — the model does its best work on a complete brief given in one turn, not assembled across a dozen exchanges. (Issue exists from `/start-phase`.)
2. **Plan it** — summarize what you're going to do. Wait for explicit approval before writing code or running commands.
3. **Cut the branch** — once approved: `git checkout -b task/X.Y-short-description`.
4. **Prove it first** — when behaviour is changing, the check comes before the change: write it, run it, and watch it fail *for the reason you expect*. That failure is what proves the check bites; one written afterwards has never been observed failing, so it may be asserting nothing. The check must exercise the thing named in its own title — a test named for one thing that calls another is worse than none, because it turns an unverified claim into an apparently-verified one. **What counts as a check here is the `Proof` slot in `.claude/CLAUDE-context.md` § Workflow Mechanisms.**
5. **Build it** — until the test passes. If you find yourself writing code first and then reconstructing the proof by deleting it to watch the test fail, you have done step 4 the long way round.
6. **Run the proof** — the checks covering what you touched, not the whole suite; the full suite is my call, never automatic. **The test is coverage, not confidence:** if the checks you already ran exercise the files you changed, that is the whole proof and you are done. Running everything again *because you are about to hand back*, or to see green one more time, is the banned case — and it is the one that actually happens, because "I'm finishing" feels like a reason and isn't one. If a change plausibly reaches code you can't name (a shared fixture, a seed, a config every suite loads), say that out loud and ask; don't run the suite to find out. **Command: the `Proof command` slot in `.claude/CLAUDE-context.md` § Workflow Mechanisms.**
7. **Check the surface** — confirm the change is right where a person actually meets it, which a passing check does not tell you. **How: the `Surface check` slot in `.claude/CLAUDE-context.md` § Workflow Mechanisms.**
8. **STOP. The task is built, not shipped.** Report what changed and what passes, then **stop and wait**. Do not commit, do not push, do not open a PR, do not start the next task. This is a hard stop, and it is the point of the whole workflow: it is where I look at the work. Waiting is the correct end of a build turn — including when everything is green, the next task is obvious, and stopping feels like leaving something unfinished. It isn't. Handing back *is* the finished state.
9. **`/kill-this` — I invoke it, you don't.** It commits, pushes, runs `@code-review`, opens the PR with `closes #<issue>`, and appends a `## Task <N>` block to the session file (on the orphan `sessions` branch). Run per task; multiple per session. **Reaching the same end state by hand is not the same thing and is never acceptable** — a hand-typed `git push` + `gh pr create` produces a PR that looks identical and has never been read by `@code-review`. That is the only automatic read of the diff before it merges, and its absence announces itself to nobody. If you believe a task is ready, say so and stop; that belief is not a trigger.
10. **Pick up another task or close out** — start step 1 with a new branch, or run `/its-dead` once at the end of the Claude window. Merge PRs whenever — order doesn't matter.

**No proof, no push.**

**Steps 4, 6 and 7 name a slot rather than a tool** (DEC-S042). The shell states what the step must achieve; `.claude/CLAUDE-context.md` § Workflow Mechanisms says how it is done here. They are filled, not overridden — there is no default to correct, and nothing cites a step *number*, because numbers move and a stale cross-reference in an always-loaded file fails silently.

**An unfilled slot is a real answer and must be written as one.** `Surface check: none — no human-facing surface` is a fact a reader can check. Leaving it blank is not.

## Migration Protocol

- **All schema changes go through migrations.** No exceptions. Migrations are the source of truth — never edit schema through a dashboard on any environment, and never hand-patch an already-applied migration.
- **Before creating a migration:** check for open PRs touching the same tables (`gh pr list`). If overlap exists, merge the in-flight PR first, or rename the new migration to a later timestamp to keep ledger order clean.

The project's migration **toolchain** — CLI commands, production-write protection (DEC-S009), and Supabase↔Vercel env-var sync — lives in `.claude/CLAUDE-context.md` under `## Migration Protocol (project)`. Projects without a database mark it `N/A` there.

## Conventions

Project coding conventions — typing, component structure, data fetching, auth/RLS, error-handling contract, naming, UI/brand, and testing layout — live in `.claude/CLAUDE-context.md` under `## Conventions`. They're stack-specific, so they're project-owned.

## Decision Record (DEC-S036)

**One decision, one file.** Each lives at `docs/decisions/DEC-<id>-<slug>.md` with frontmatter carrying `id`, `title`, and `topic`. `docs/DECISIONS.md` is a **generated** topic index over them — editing it by hand is a wasted edit that `check:decisions` will reject.

**Reading.** Read one decision by reading its file: `grep -rl DEC-NNN docs/decisions/` resolves any id, and `grep -rl 'topic: "Auth' docs/decisions/` pulls a whole topic. Don't load the whole record to answer one question, and **don't cite a decision you only saw in the index** — the index carries titles, not holdings, and a confident citation of a decision you didn't read is how a stale answer gets laundered into a fact.

**Writing — search before you write, every time.** Name the subject, search the record for it, and **say what came back**:

```
grep -rli "<subject>" docs/decisions/
```

- **A decision on that subject exists → you are amending it.** Open that file. This is the common case and gets more common as the record matures.
- **Nothing comes back → new decision.** Next id after the highest in `docs/decisions/`. Then `npm run gen:decisions`.
- **Several come back → amend the one your change is *about*.** Not every file that mentions the word. Ask which decision would be wrong if you shipped this; that is the one. The others get a **see also** if a reader of them would be misled without it, and nothing otherwise.
- **Partial overlap → amend the part you change, and say so.** A change that alters one leg of a decision is still that decision, later. If it genuinely changes two subjects, that is two amendments in two files, not one new decision covering both.

State the search result in the PR — *"`grep -rli deposit` returned DEC-NNN; this changes its posture, so it amends"*, or *"nothing on rate limiting; new id."* **That sentence is the whole control.** A session that has to write "DEC-NNN covers deposits and this is not that" cannot do it when it's false, and no definition of "amendment" catches what that catches.

**An amendment goes in the decision's own file**, appended at the bottom:

```markdown
## Amendment, YYYY-MM-DD (who) — one line on what changed

**What this changes, and what still stands.** Then context, decision, why.
```

Say what still stands. An amendment that only states the new position leaves a reader guessing which parts of the original survived — and the original is not edited or struck through, so both remain readable in order.

**There is no new decision that amends an old one.** If it changes what an existing decision decided, it is that decision, later — not a new subject. A new id is for something worth writing even if nothing before it existed. Two decisions that merely relate carry a plain **see also**, named in both files.

**What this protects:** one place per subject, so *"what did we decide about X"* has one answer; and a session reading the one file it needs rather than a monolith — which four files on one subject defeats just as thoroughly as one file holding everything.

**A decision that changes `SPEC.md` still declares it in frontmatter** — this part is unchanged and is not the same mechanism as the retired `amends:`:

```yaml
amends_spec:
  - section: "2.4"             # a NUMBERED section of docs/SPEC.md
    scope: "the availability rule; the surface below is unchanged"
```

The generator writes the pointer under that spec section's heading — never hand-write it — and the gate fails the build if the claimed spec edit never landed. That check exists because unlanded spec claims were the largest single finding class in the audit behind DEC-S036, and nothing else catches one. Declare it from an amendment section the same as from a new decision.

**The index is a list of subjects, not a summary of what is current.** An in-file amendment leaves the index row showing the original title and date. The current answer is in the file.

**The gate.** `npm run check:decisions` fails on a stale index, a duplicate id, an unknown topic, a dangling reference, and a declared spec amendment that never landed. Its siblings `check:context` and `check:docs` cover the always-loaded context files and the rest of the doc set. All three run before the slow stages of `verify` — they fail in milliseconds. Project-specific knobs live in `docs/decisions/_config.json` and `.claude/doc-check.json`; the scripts themselves are shared and identical everywhere, so don't edit them per-project.

## Session Skills

| Skill | When | What |
|-------|------|------|
| `/its-alive` | Session start | Ensure `.sessions-worktree/` exists, open per-session file on orphan `sessions` branch, capture transcript, read context, recommend task |
| `/pause-this` | Mid-session break | Build check, commit WIP on task branch, note pause in session file (sessions branch) |
| `/restart-this` | Resume from pause | Reload context, continue same session |
| `/kill-this` | **Per task** (DEC-S013) | Build check, commit code on task branch, open PR, append `## Task <N>` block to session file. Run N times per session — one per task. No time math. |
| `/its-dead` | Session end (once per window) | Stamp `ended:`, tally points, display wall_clock to screen, close session file. No time math, no version bump (those moved to `/retro`). Merge PRs whenever — order doesn't matter. |
| `/start-phase` | Phase boundary (start) | Materialize phase as Issues with `phase:N`, `points:X` labels |
| `/retro` | Phase boundary (end) | Compute per-session active time (wall − breaks) from `started`/`ended` + transcript break inference. Aggregate one phase velocity (active h/pt). Mark `[x]`, write retro, patch-bump per merged PR + minor-bump at close. |
| `/bump-major` | Breaking change | Manually bump major version. CHANGELOG.md entry + tag on the trunk (`main`). Dev projects only |
| `/promote-production` | Ship trunk to prod | ff-merge `main` → `production` (deploy-only; tag already on the commit), push. Projects with a `production` branch only |
| `/read-the-tape` | `--queue` on a weekly-ish cadence; bare to audit one session now | Audits session JSONL and writes one cited observation per session to seeds. **Changes nothing here** (DEC-S040). Drain mode works through whatever the `SessionEnd` capture hook queued (DEC-S045). Needs a resolvable seeds checkout |
| `/doc-consistency-check` | Ad-hoc, when docs feel drifted (no scheduled trigger) | Cross-reference factual claims across `docs/*.md` + root `CLAUDE.md`; flag mismatches + unfilled placeholders. Report-only via @doc-consistency |

**Dev identity:** `~/.claude/devname` (one-line file with handle, e.g. `eric`). Set once per machine.

**Task model:** PROJECT_PLAN.md is read at planning, written at retro. Untouched mid-phase. Current-phase tasks live as GitHub Issues. The phase ends when its issues close.

**Workflow fixes don't get made here (DEC-S039, DEC-S040).** A skill or shared agent that misbehaves in this project is **not** fixed in this project. Those files are canonical in seeds, and there is no sync in either direction any more — so a local fix does not get overwritten, it just never goes anywhere. It becomes invisible drift in a file that is meant to be identical across every project, and nothing will ever reconcile it.

The route that ends somewhere: `/read-the-tape` records the failure as a cited observation on seeds' `observations` branch. `@workout` runs periodically in seeds, judges what has accumulated across every project, and promotes what earns it into the templates. Then someone copies the merged change back out, by hand.

**You do not have to remember to start that route (DEC-S045).** A `SessionEnd` hook on the machine copies each ending session's transcript into a local queue, so the evidence survives whether or not anyone thought the session was interesting — which matters, because transcripts are deleted after `cleanupPeriodDays` (default 30) and the sessions worth auditing are usually the ones nobody suspected. `/read-the-tape --queue` distils the backlog later. The hook is installed per machine, in the **user** settings file (`~/.claude/settings.json`), not in this repo — and it captures nothing from a cloud-container session, whose filesystem dies with it.

**Nothing here is exempt.** `/read-the-tape` no longer applies even the small local fixes it used to — `.claude/settings.json` permission entries included. It observes and writes one file to seeds; that is all it does. Fixing anything in this repo is your call, made deliberately, not something an audit does on its way past.

## Agents

| Agent | Model | When | Purpose |
|-------|-------|------|-------|
| @architect | Opus 5 | Before design decisions, new dependencies, scope creep | Coherence vs SPEC + DECISIONS |
| @code-review | Sonnet | After every commit (wired into `/kill-this`) | Catch issues early |
| @pm | Sonnet | Start/end of sessions via skills | Track progress, flag risks |
| @ui-reviewer | Sonnet | After UI work, phase boundaries | Design quality |
| @tape-reader | Sonnet | `/read-the-tape` | Audits session JSONL for workflow anti-patterns. **Observer** (DEC-S040) — writes one cited observation to seeds and changes nothing in this repo |
| @doc-consistency | Sonnet | Via `/doc-consistency-check` skill, or ad-hoc | Cross-reference factual claims across project docs; flag mismatches + unfilled placeholders. Report-only |
| @ideas | Sonnet | Park an idea, re-rank, or audit the parking lot | Curate docs/FUTURE_IDEAS.md — capture, dedupe, cross-ref, keep the index. Edits only that file, and creates it on first use |

## Model Selection

Default to the cheapest model that does the job. **Opus 5 is the standing model** for development and architecture; **Sonnet** handles cheap/scoped work. **Fable is rarely worth it** — on agentic coding at `max` effort, Opus 5 lands within half a percent of Fable's peak at half the cost per task, so the frontier tier is a narrow exception, not a standing escalation path.

| Tier | Model | $/MTok (in/out) | Use for |
|------|-------|-----------------|---------|
| Cheap | `claude-sonnet-5` | $3 / $15 | Trivial/scoped agents and reviews — fast, low-cost. |
| Default | `claude-opus-5` | $5 / $25 | The standing model for development and architecture. Most work runs here. |
| Frontier (rare) | `claude-fable-5` | $10 / $50 | Reach for it only after Opus 5 at `max` has actually failed the task. Not a routine escalation. |

- **Spec it fully, then let it run.** Opus 5's edge is largest on long, coherent, multi-file work handed the *complete* specification in one turn. Assembling the spec across interactive turns costs both quality and tokens. This is the highest-leverage habit change — it makes Micro Workflow step 1 load-bearing rather than ceremonial.
- **`effort` is the primary lever, and it sweeps down.** `effort` (`low`/`medium`/`high`/`xhigh`/`max`, via `output_config`) buys quality more cheaply than a model jump. Start at `xhigh` for coding/agentic work and `high` elsewhere, then **try lower** — `low` and `medium` are unusually strong on Opus 5, and effort is what spends the usage allowance. `max` only when correctness must beat cost.
- **Fast mode** runs ~2.5× faster at 2× the price ($10 / $50). A deliberate choice for a specific impatience, never a default.
- **File memory is a force multiplier.** Session files, `design/`, `docs/DECISIONS.md`, and acceptance criteria are the persistent notes the model exploits to improve its own output. Keep them current and reference them explicitly.
- **Agents:** model in agent frontmatter. `@architect` runs Opus 5. Reviewers (`@code-review`, `@pm`, `@doc-consistency`, `@tape-reader`) and `@ui-reviewer` stay Sonnet. The `model: opus` frontmatter alias resolves forward on its own — no per-release edit needed.
- **New agents:** default to Sonnet; pin `model: opus` only when the agent's standing job needs it.

## PR Workflow

- Each task gets a branch: `git checkout -b task/X.Y-short-description`.
- Issues assigned to phase via `phase:N` label (created by `/start-phase`).
- PR title references issue: `closes #N`.
- `/kill-this` opens PR. Self-merge after review unless stakeholder review needed.
- Keep ≤3 open PRs. Prefer 1.
- Never two open PRs with migrations on the same table — merge one first.
- **Stacking PRs is preferred** when tasks depend on each other. Branch the next task off the previous task branch (`git checkout -b task/X.Y-next task/X.Y-prev`), not off main. Only wait for the previous PR to merge when there's a migration conflict on the same table.

### Production branch (DEC-S022)

`main` is the always-active trunk. Every task PRs into `main`; `/retro` patch-bumps per merged PR + minor-bumps at phase close, tagging on `main` immediately. This is the same workflow whether or not the project deploys.

Deployable projects add a `production` branch — a downstream deploy pointer the host (Vercel, etc.) watches. It is **never** a PR base and is never touched by the sync. Ship with `/promote-production`, which ff-merges `main` → `production` and pushes (the version tag is already on the commit from the bump — promotion does not tag).

Adopting a production branch:
```
git checkout -b production main && git push -u origin production
```
Then repoint the host's production branch from `main` to `production` (e.g. Vercel → Settings → Git → Production Branch) — **before** `main` takes active work, or WIP auto-deploys to prod. Removing it: delete the branch and point the host back at `main`. No skill changes to opt in or out — only `/promote-production` cares (it gates on `origin/production`).

## Versioning

Every dev project carries a SemVer version in `package.json`, mirrored to a git tag (`vX.Y.Z`) on `main`.

**Three triggers:**
- **Patch:** on projects with a `production` branch, `/promote-production` bumps + tags on each ship — one release = one patch (a `main` HEAD that's already a fresh tag ships as-is). On projects that deploy straight off `main` (no `production` branch), `/retro` Step 8.2 patch-bumps per merged PR instead.
- **Minor:** `/retro` Step 8.3 — at phase close, after any patches (Y+1, X=0). CHANGELOG entry summarizes the phase.
- **Major:** `/bump-major` manual. User supplies the breaking-change rationale.

**Tag rule:** all tags are applied on the active trunk (`main`) at bump time (DEC-S022) — by `/promote-production` (patch, on ship), `/retro` (minor), or `/bump-major` (major). `production` only ever receives an already-tagged `main` commit via ff-merge.

**Detection:** these skills check the repo root has a `package.json` **with a `version` field** before bumping — `node -e "process.exit(require('./package.json').version ? 0 : 1)" 2>/dev/null || echo "not versioned"`. If there's no manifest, or one without a version, they no-op silently.

The gate is the field rather than the file because "has a `package.json`" was only ever a proxy for "is a versioned thing", and the two came apart as soon as a repo wanted a test runner without a version. A project that ships software has both, so nothing changes for it.

### Deploy + review reference

The `<VersionTag />` wiring (login + footer, and the `NEXT_PUBLIC_` gotcha that silently renders `v0.0.0`), the CHANGELOG format, and the phone PR-review notes are reference material, not standing rules — they belong out of the always-loaded shell, in the deploy reference this project lists under `## Additional Docs` in `.claude/CLAUDE-context.md`. Deployable projects install one from the seeds template; a project that doesn't deploy has none, which is why the path is named there and not here.

## Workflow Notes
- **Diagnostic commands** (build, lint, type check, test): run directly — see errors, fix them, don't bother the user.
- **Environment-changing commands** (npm install, supabase migrations, git push, deploys): output these for the user to run.
- **Never rebase a task branch that already has commits on origin.** If main has advanced while a PR branch is open, leave the branch as-is — GitHub's "Update branch" button handles this at merge time. Rebasing rewrites remote history and requires a force-push. Use `git merge --ff-only` only if explicitly asked.
- **On a surprise or mismatch, reconcile before diagnosing.** Pin the assumption and the environment first — dev vs prod, which DB, is the server even up — before chasing a theory or building. One environmental check ("can you run the suite right now? what env vars are set?") beats a multi-step debug built on an unchecked premise.
- **JSON parsing in Bash:** Prefer `gh ... --jq '...'` (built-in jq via `gh`) or `jq` over `python3 -c "import json,sys; ..."` one-liners. The python invocations trigger per-pattern permission prompts (each unique argument list is a new allowlist entry), while `gh --jq` runs under the existing `Bash(gh ...)` allowance. For non-`gh` JSON, install/use `jq` directly. Reserve python for cases where the data shape genuinely needs control flow.
- **`npx` is denied fleet-wide — run a locally-pinned binary as `npm run <script>` or `./node_modules/.bin/<bin>`.** The deny entry is `Bash(npx *)`; DEC-S023 governs the mechanism (default-allow plus a deny guardrail, and the precedence rule below) rather than this specific line. It exists because the *same syntax* fetches an arbitrary package off the network and runs it against the repo; nothing in the command string distinguishes that from invoking a devDependency you already installed, so the pattern cannot be narrowed to catch only the dangerous half. **`deny` beats `allow`, so no project can allowlist its way out** — an added `Bash(npx playwright *)` does nothing. The direct path is the route, and it works: verified in a project where `npx vitest` was refused and `./node_modules/.bin/vitest` ran unchanged. Prefer an npm script, because it survives someone reading the docs a year later. **Docs that spell commands as `npx <thing>` are the real trap** — they read as sanctioned, and the failure is a permission refusal rather than an error, so it looks like the agent being difficult rather than the doc being stale.
- **A scripted edit must fail loudly when its anchor doesn't match — and a one-line `sed -i` is a scripted edit.** That last clause is not padding: one observed session wrote a `python3` script with `assert n == 1` before writing, correctly, and ninety minutes later ran a bare `sed -i 's|old|new|g' file` with no check at all. The rule reads as though it is about *scripts*, and a one-liner doesn't feel like one. `Edit` refuses to write when its target string is missing or ambiguous; a `read_text()` / `.replace()` / `write_text()` script, or a `sed -i`, writes the file back unchanged, prints nothing, and exits 0. Applying a mechanical change across many files with one script is a legitimate choice — reproducing the same anchor by hand ten times has its own failure mode — but only if the script asserts the match count per file and exits non-zero on zero matches. Without that, "done" means the script ran, not that the change landed, and the file it silently skipped looks reviewed.
- **Read files with the Read tool — never `sed`, `grep`, `awk`, or `cat` to pull a section out.** Read is allowlisted and never prompts. A shell one-liner extracting a section can miss an allow-pattern match and stop a skill dead on a permission prompt mid-run, which has now happened twice on `.claude/CLAUDE-context.md` — once in `/kill-this`, once in `/promote-production` — in a session whose allowlist carries `Bash(*)` and that prompted for nothing else. Reading the whole file costs less than one interruption. `grep` to *search* across many files is fine. The banned shape is sed-ing a section range out of one file whose path you already know — the thing Read does without a prompt. **`Bash(sed -n *)` is therefore denied fleet-wide** — this paragraph on its own did not hold: audits found 58 violations across three sessions and two repos in one week, none of which announced itself, because the harm only lands on the intermittent allow-pattern miss. Reach for `Read` with `offset`/`limit`; a denial here means you wanted `Read`, not a differently-shaped `sed`.
- **A denied command is a decision, not a syntax error.** Whether the deny came from the permission rules or from me clicking through, re-issuing the same intent in a new shell shape — chained, bare, with a different flag — is trying to route around the answer. Once: fine, the deny message doesn't say why and the second attempt is a fair guess. Twice on materially the same command: stop and ask what the denial means. One observed session re-shaped a denied `git branch -D` five times before stopping, then reported the count to me as "twice."
- **Never write a bare `#N`. Always say which kind: `issue #699`, `PR #707`.** GitHub allocates issues and PRs from **one shared counter**, so the two sequences interleave and stay permanently adjacent — `#699` is an issue, `#707` is a PR, and nothing in the number tells you which. There is no way to separate them: they are drawn from the same sequence at creation, and burning numbers advances both. So the prefix is the only fix, and it costs one word. Applies everywhere the number is written — PR bodies, issue text, commit messages, decision records, session files, and chat. The one exception is `closes #N` in a PR body, which is GitHub syntax and must stay bare to work.
- **The env template is `env.example` — no leading dot (DEC-S043).** Everything matching `.env*` is a secret, with no exceptions to remember: `.gitignore` is one line (`.env*`, no `!` negation), and the permission deny is one blanket per tool (`Read`/`Write`/`Edit` on `**/.env*`). The dotted name is the near-universal convention and a scaffold or a contributor will keep recreating it — rename it back out of the namespace when they do. This exists because an *enumerated* deny list can only name secrets someone thought of in advance, and the two files it missed in practice were a hand-made `.env.local.backup` and a per-vendor `.env.xola.prod`.
- **Bug reports:** create a GitHub issue, label `bug`, add to current or next phase.
- **Don't guess third-party API shapes** from naming or 403/404 signals — stop and ask for the official docs; never write code against a guess.
- **Context docs carry decisions, rationale and pointers — never inventory.** `CLAUDE.md` and `.claude/CLAUDE-context.md` load into every session as ground truth, so a stale sentence in them is believed and acted on rather than checked. Rationale ("webpack, because Turbopack lacks `extensionAlias`") doesn't rot. A **snapshot of current state** ("the adapters are X and Y; Z comes later") is stale the day the code moves — and no doc-consistency audit catches it, because the claim is false against **code**, the corpus doc sweeps never read. Write a pointer instead: `ls <dir>/*-channel.ts` sends the reader to the truth rather than copying it, and it is checkable — and note the angle brackets, which mark this as an illustration rather than a claim about this repo. A worked example written as a real path is a dead reference in every project that copies the shell. `dev/claude/scripts/check-context.mjs` asserts every repo path and glob those two files cite still resolves — wire it into the project's verify chain. It cannot judge a *characterization*; "X is the live transport" is a sentence only a reader can validate.
  - **An env-overridable number is not a fact a repo can state.** "Currently 30 days" for a value read from env is a claim about a *deployment*, unanswerable from a checkout. Cite where the constant is defined and say the deployed value lives in the host's env.
  - **Before asserting what is built or live, check the code in the same turn** — one `ls` or `grep`. This rule exists because a session read "SMS = later swap" from a context file, filed an issue declaring a feature blocked on an adapter that had shipped weeks earlier, and explained the blockage at length. The doc was wrong; the failure was not verifying a live-state claim that took one command to check.
  - **It binds hardest on anything outside this checkout** — a sibling repo, another project's decisions, a machine's global config. That is where the habit doesn't fire, because none of it looks like "the code", and it is also where nobody downstream can check you: three observed sessions asserted a sibling repo's user base, a sibling repo's chosen architecture, and a monitoring system's coverage, all as settled fact, none from a file read. Each was wrong; each was caught only because the operator happened to have the knowledge personally. Sibling repos are on disk — open the file at its path, or say you haven't. Reading a source is not the same as characterizing it correctly, either: "named as an assumption inside a decision about something else" is not "the planned direction," and the gap between those two is invisible once it's written down as prose.
  - **A document meant to be read without me present raises the cost from wrong to irreversible.** A handoff brief, a research doc, a runbook — anything whose own header says it's for a fresh session — carries its claims onward as ground truth with nobody left to correct them. In one, every sentence about another repo names the file it came from, or it doesn't go in.

Project-specific debugging gotchas (dev-server checks, stale-process traps, auth-redirect quirks) live in `.claude/CLAUDE-context.md` under `## Workflow Notes (project)`.

## Approval Before Action (all tasks)

For every task — bug, feature, or question — explain the plan and wait for my go-ahead before doing anything:
1. State what you'll create or modify and why, and list the commands you'll run (commits, pushes, installs, anything touching production).
2. For a bug or question: explain the cause and your proposed fix first.
3. Wait for "go", "do it", or equivalent. Don't edit files or run commands until approved.

**Answering a question you asked is not approval.** This is where "or equivalent" gets abused, and it is the observed failure, twice in one session and twice again in another. A scoping answer ("both of those should have entry points"), a preference between options you offered, and a refusal to decide ("I'm not a sysadmin") all tell you *what* the thing should be. None of them says *start building it*. Approval is a reply to the plan you wrote in step 1 — so if no plan was written, nothing said since can have approved one. When the register is collaborative and fast and we're clearly agreeing, that is exactly when this goes wrong: agreement about the shape of a thing is not consent to spend an hour building it.

**Working through a numbered document — a runbook, a migration plan, a checklist — is not the ordinary task loop.** Each step is its own cycle: present the step, wait, do the step, wait again before commit or push. Do not fold investigate → edit → commit → push into one turn because the step is numbered and looks atomic. The one time this rule was given in-session as an explicit instruction, it was violated within three hours and had to be restated verbatim.

**Trust my statements the first time.** "It's fixed" / "it's done" is a fact, not a request to re-verify or keep digging. Register a decision I've stated as settled — verify at most once, never re-raise it later as a "gap." Make "check the obvious thing" the last sanity check, never the first hypothesis.

## Scope Discipline
Check `docs/SPEC.md` "Not V1" before adding anything. Apply a change only to the surface I named — don't propagate it to sibling pages, and never invent or misattribute a rationale I didn't state (especially in DECs and durable notes).

If a task feels bigger than its estimate: stop, re-estimate, update PROJECT_PLAN.md (next phase boundary, or via Issue mid-phase); if it's scope creep, flag it and move on.

**A workflow rule needs an observed failure behind it.** If you can't cite the session, transcript, or PR that produced it, it's a proposal — say so. A rule that sounds right and was never triggered by anything gets skimmed past forever after.

**Prefer removing.** A retired rule with a decision explaining why it went is worth more than a new one.

**Splitting is a reviewability call, not a capability one.** Points size estimation; they don't cap how much ships in one run.
- **Don't split a coherent 8** (one feature, one migration, one subsystem) just to honor a ceiling — run it as one unit with the full spec up front.
- **Do split** when the diff is too big to review well, the blast radius or reversibility worries you, there's a migration conflict, or an "8" is secretly two unrelated things.
- **Still break genuine 13s** — for review and risk, and because a 13 usually means *I* don't understand it well enough yet. Both reasons are human-side; neither is about what the model can hold. Points stay 2/3/5/8/13 — a bigger unit of work is a bigger *run*, not a bigger number, and inventing a new bucket would break velocity comparability with every prior phase.

## Tone
Occasional dry humor and sarcasm welcome. One good line beats three forced ones.

## Communication

**Register — length, shape, preamble, when to expand — is set by the `Concise` output style, not by this file** (DEC-S050). Set `"outputStyle": "Concise"` in **user settings** (`~/.claude/settings.json`) — it's a machine preference, so one edit covers every repo on the box and a new checkout inherits it. To override it for one repo — `Explanatory` while designing, say — set it in that repo's `.claude/settings.local.json`, which takes precedence. Needs Claude Code v2.1.237+, and takes effect at the next session start, never mid-session. The rules below are the ones `Concise` says nothing about.

**Do not re-add register prose here.** This section was 976 words of it — a tag on every reply, four named reply kinds, a length permit for the hard ones — and it worked sometimes. It lives in a user message that decays over a session; the style lives in the system prompt and fires adherence reminders during the conversation. If `Concise` turns out to be missing something, the answer is a custom output style, which **replaces** the built-in rather than supplementing it — not another paragraph in this file.

**Within a session, just ask** — `Concise` answers in full when you ask for detail. Changing style is a per-session act: set it, then start a new session; it is read once at session start and never applies mid-turn. `Explanatory` suits design and planning, where you want insight volunteered rather than requested.

**Never lead with a false premise.** If you don't know the cause, ask — "is the server up? which DB?" is one line and fair. What's banned is stating a made-up cause as fact and explaining at length on top of it.

**Ask in prose. Never use the `AskUserQuestion` tool.** A branching decision with three named options and a recommendation is a fine *question* and a bad *picker* — write it out and let me answer in words. A standing preference across every repo, written here because this is the only place a session can see it.

**Cite facts; label proposals.** Any claim about the code, config or project rules cites a file:line or a tool result. If you can't cite it, ask instead of asserting. This never restricts *ideas* — propose freely, just mark them "proposed / not in the codebase". Inventing a fact is fabrication; a labelled proposal is not.

Keep adaptive thinking on — reasoning stays in the thinking block and the reply stays clean.

## Cost and Waste

Never minimize cost. Banned phrasings include but are not limited to:
- "essentially zero"
- "negligible"
- "only a few cents"
- "just X dollars"
- "a rounding error"
- "not a big deal"
- "don't worry about it"

If you find yourself reaching for one, stop. Any synonym counts. If the function of the phrase is to minimize, it's banned.

It's my money. Willing-to-spend is not the same as willing-to-spend-flippantly. Treat every cost as real, including small ones. Same rule for compute, API calls, third-party services, and dependencies — anything that consumes resources I'm paying for.

Waste of any kind — food thrown out, hours lost, a bad batch, a bricked migration, an over-provisioned instance, a wrong dependency pulled — is a fact, not a problem to console me about. When I tell you something had to be discarded, do not reassure me it's fine. Acknowledge it and move on.

If you catch yourself about to write a reassurance, just don't. The fact is the fact.
