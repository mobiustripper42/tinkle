---
name: promote-production
description: Promote the active trunk (`main`) to the `production` deploy branch via fast-forward merge, then push. The release was already version-bumped and tagged on `main` by `/retro` or `/bump-major`, so promotion is deploy-only — it advances `production` to the tagged commit. Use when work on `main` is ready to ship. Requires `origin/production` to exist (DEC-S022).
tools: Read, Edit, Write, Bash, Grep
---

You are promoting `main` → `production`. Under DEC-S022, `main` is the always-active trunk and `production` is a downstream deploy pointer that Vercel (or whatever host) watches. Version bumps and tags already happened on `main` at `/retro` / `/bump-major`; this skill does **not** tag — it ff-merges the trunk into `production` and pushes, which is the deploy moment.

## Step 0 — Sanity gates

**Production branch exists:**
```
git show-ref --verify --quiet refs/remotes/origin/production || echo "missing"
```
If `origin/production` doesn't exist (locally cached), STOP. Tell the user: "/promote-production requires an `origin/production` branch (DEC-S022). This repo deploys straight off `main` — there's nothing to promote. To adopt a production branch: `git checkout -b production main && git push -u origin production`, then point the host's production branch at it." Do not proceed. Step 1 refreshes the cache from the remote regardless, so a stale local view that misses a freshly-created `production` is recovered after the user re-runs.

**Clean working tree:**
```
git status --porcelain
```
If non-empty, STOP. Tell the user: "Uncommitted changes — commit, stash, or discard before promoting." Wait.

## Step 0.5 — Confirm the release was QA'd

If the project deploys the trunk (`main`) to a shared staging or preview environment before production, confirm it was actually QA'd against **this** release before promoting. The environment and what to check are project-specific — they live in `.claude/CLAUDE-context.md` (`## Migration Protocol (project)`) or `docs/DEPLOYMENT.md`. Ask the user:

**"Has the staging/preview environment been QA'd against this release since the last merge? (yes/no)"**

If no, STOP and tell them to QA first. Do not proceed on an unconfirmed answer. If the project deploys straight to production with no separate staging surface, skip this step.

## Step 1 — Sync local refs

```
git fetch origin main production --tags
```

## Step 2 — Check ff-mergeability

A fast-forward is possible iff `production` is an ancestor of `main`. Check:
```
git merge-base --is-ancestor origin/production origin/main && echo "ff" || echo "diverged"
```

**ff:** continue to Step 3.

**diverged:** STOP. `production` has commits that `main` doesn't have (someone committed to `production` directly, or rebased). Show:
```
git log --oneline origin/main..origin/production
```
Ask: **"production has diverged from main. Options: (a) merge production into main first (preserves both histories), (b) abort and resolve manually. Which?"** If (a), guide the user through merging `production` → `main` on a branch (likely needs a PR into `main`), then ask them to re-run /promote-production once that lands. Never auto-resolve.

## Step 3 — Identify the release being shipped

```
git fetch --tags
SHIP_SHA=$(git rev-parse origin/main)
SHIP_TAG=$(git tag --points-at "$SHIP_SHA" | grep '^v' | sort -V | tail -1)
```
`SHIP_TAG` is the version tag already on the trunk HEAD (created by `/retro` or `/bump-major`). If `SHIP_TAG` is empty, surface a warning — promotion still works, but the promoted commit carries no version tag: **"⚠ No `v*` tag on `main` HEAD. Promote anyway (deploys an untagged commit), or bump first via `/retro` / `/bump-major`? (promote/abort)"** Wait. Do **not** invent or apply a tag here — tagging is the trunk skills' job (DEC-S022).

## Step 4 — ff-merge production → main HEAD

```
git checkout production
git merge --ff-only origin/main
```

If `git merge --ff-only` fails (rare race — `main` moved between Step 2's check and now), STOP. Surface the error and ask the user to re-run.

## Step 5 — Push

```
git push origin production
```
The tag, if any, is already on the remote (pushed by the trunk skill that created it). Only push tags here if Step 1's fetch revealed a local tag the remote lacks:
```
[ -n "$SHIP_TAG" ] && git push origin "$SHIP_TAG" 2>/dev/null || true
```

If the `production` push fails, STOP. Surface the failure with full output. Local state is consistent (`production` advanced locally) but the remote isn't yet — retry the push once the issue (auth, network, branch protection) is resolved.

## Step 6 — Summary

```
Promoted main → production at <SHIP_TAG or short-SHA>
production now at <short-SHA>
Host deploy on `production` triggered (if the host watches the production branch).
```

Remind the user to verify the deploy: tap the production URL, confirm the version tag in `<VersionTag />` displays the shipped version.

**Branch hygiene:** do NOT delete `main` or `production` — `main` keeps accumulating the next batch of work; `production` waits for the next promotion. Both are permanent.
