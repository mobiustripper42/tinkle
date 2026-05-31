---
name: promote-staging
description: Promote staging to main via fast-forward merge. ff-merges `staging` into `main`, tags the release with the current `package.json` version, and pushes both branches plus the tag. Use when accumulated work on `staging` is ready to ship. Requires `origin/staging` to exist (DEC-008).
tools: Read, Edit, Write, Bash, Grep
---

You are promoting `staging` → `main`. This is the deploy moment for staging-flow projects (DEC-008).

## Step 0 — Sanity gates

**Staging exists:**
```
git show-ref --verify --quiet refs/remotes/origin/staging || echo "missing"
```
If `origin/staging` doesn't exist (locally cached), STOP. Tell the user: "/promote-staging requires `origin/staging` (DEC-008). This repo doesn't have a staging branch — promotions go straight through `/its-dead` on `main`." Do not proceed. Step 1 will refresh the cache from the remote regardless, so a stale local view that misses a freshly-created staging is recovered after the user re-runs.

**Dev project (for tagging):**
```
[ -f package.json ] || echo "missing"
```
If `package.json` is missing, STOP. Tell the user: "/promote-staging tags with the version in `package.json` (DEC-007). This repo has none — promotion would be untagged. Aborting." (If the user genuinely wants an untagged ff-merge, they can do it manually.)

**Clean working tree:**
```
git status --porcelain
```
If non-empty, STOP. Tell the user: "Uncommitted changes — commit, stash, or discard before promoting." Wait.

## Step 1 — Sync local refs

```
git fetch origin main staging
```

## Step 2 — Check ff-mergeability

A fast-forward is possible iff `main` is an ancestor of `staging`. Check:
```
git merge-base --is-ancestor origin/main origin/staging && echo "ff" || echo "diverged"
```

**ff:** continue to Step 3.

**diverged:** STOP. `main` has commits that `staging` doesn't have (someone hot-fixed main directly, or rebased). Show:
```
git log --oneline origin/staging..origin/main
```
Ask: **"main has diverged from staging. Options: (a) merge main into staging first (preserves both histories), (b) abort and resolve manually. Which?"** If (a), guide the user through merging main → staging on the staging branch (likely needs a PR), then ask them to re-run /promote-staging once that lands. Never auto-resolve.

## Step 3 — Read the version that will ship

```
NEW_VERSION=$(npm pkg get version | tr -d '"')
```
`npm pkg get` is JSON-aware. If `NEW_VERSION` is empty, STOP and surface "Could not parse version from package.json — is the file valid JSON with a `version` field?"

This is whatever `/its-dead` (patch), `/retro` (minor), or `/bump-major` (major) accumulated on `staging` since the last promotion.

Check the tag doesn't already exist (would mean previous promotion succeeded but failed to advance the version):
```
git rev-parse "v$NEW_VERSION" 2>/dev/null
```
If the tag exists, STOP. Tell the user: "Tag v$NEW_VERSION already exists — `staging` hasn't been bumped since the last promotion. Either bump first (e.g. `/its-dead` after a merge, or `/retro` for a phase close) or investigate why staging has no new commits."

## Step 4 — ff-merge + tag

```
git checkout main
git merge --ff-only origin/staging
git tag "v$NEW_VERSION"
```

If `git merge --ff-only` fails (rare race condition — staging moved between Step 2's check and now), STOP. Surface the error and ask the user to re-run.

## Step 5 — Push

```
git push origin main
git push origin "v$NEW_VERSION"
```

If either push fails, STOP. Surface the failure with full output. The local state is consistent (commit + tag exist locally) but the remote isn't yet — the user can retry the push once the issue (auth, network, branch protection) is resolved.

## Step 6 — Summary

```
Promoted staging → main at v<NEW_VERSION>
Tag pushed: v<NEW_VERSION>
Vercel deploy on main triggered (if Vercel hooks main).
```

Remind the user to verify the deploy: tap the production URL, confirm the version tag in `<VersionTag />` displays the new version.

**Branch hygiene:** do NOT delete `staging` — it's the working branch and continues to accumulate the next batch of patches. The next `/its-dead` patch bump will land on staging again.
