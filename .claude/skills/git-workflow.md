---
name: git-workflow
description: Must read before any git/gh operation. Defines AI's allowed git usage in this project — branch-only work, PR-for-master, commit message style, and forbidden destructive operations. Triggered on all git/gh tasks including commit, push, branch, merge, rebase, PR creation.
---

# Git Workflow for AI

## First Principle — Never touch master directly
**AI must never commit, push, or merge directly on `master`.** All AI work happens on a separate branch. Master changes only via Pull Request, executed when the user explicitly instructs.

## Hard Rules

| Rule | Detail |
|---|---|
| **Branch-only work** | Always `git checkout -b <branch>` before any modification. Create as many branches as needed. |
| **PR for master merge** | Use `gh pr create --base master --head <branch>` to propose changes. Never `git checkout master && git merge` directly. |
| **User-explicit approval for master merge** | `gh pr merge` only runs when the user says so ("머지해라", "merge it" 등). Do not infer or anticipate approval. See [Master Merge Execution](#master-merge-execution-user-instructed) for the exact command. |
| **Inter-branch merge is free** | Merging between non-master branches (e.g., feature → integration) is fine without asking. |
| **No force push** | `git push --force` / `git push -f` are denied by settings. Never bypass. |
| **No hard reset** | `git reset --hard` is denied. Use `git reset` (mixed) or `git restore` instead. |
| **No history rewrite on shared branches** | Don't rebase or amend commits that are already pushed to a shared branch. |

## Branch Naming Convention

| Prefix | Use case | Example |
|---|---|---|
| `feature/` | New functionality | `feature/gstreamer-integration` |
| `fix/` | Bug fix | `fix/rtsp-reconnect-leak` |
| `docs/` | Documentation only | `docs/update-git-rules` |
| `cleanup/` | Dead code, refactor | `cleanup/remove-w14-malloc-trim` |
| `chore/` | Tooling, deps, CI | `chore/bump-grpc-version` |

Use kebab-case after the prefix. Keep names short but descriptive.

## Standard Workflow

```bash
# 1. Start: branch from master (or appropriate base)
git checkout master
git pull                           # only if master moved
git checkout -b <prefix>/<topic>

# 2. Work: edit, build, test
# ... (edit files, run build/sanity) ...

# 3. Stage: be specific — never blanket `git add .` if temp files might leak
git status --short
git add <specific files>
git status --short                 # verify staged list

# 4. Commit: heredoc for multi-line
git commit -m "$(cat <<'EOF'
<subject line: type: short summary, <70 chars>

<body: what changed and why, wrapped at ~72 chars>

<optional: footer with refs, e.g., "Related: #issue">
EOF
)"

# 5. Push: -u on first push of the branch
git push -u origin <prefix>/<topic>

# 6. PR (when ready for master merge — user discretion)
gh pr create --base master --head <prefix>/<topic> \
    --title "<type>: <summary>" \
    --body "$(cat <<'EOF'
## Summary
<1-3 bullets — why>

## Changes
- file: what changed
- ...

## Test plan
- [x] build PASS
- [x] sanity (DFPS, ERROR 0)
- [ ] (other checks)
EOF
)"
```

## Master Merge Execution (user-instructed)

When the user explicitly says to merge a PR, use this default:

```bash
gh pr merge <PR#> --squash --delete-branch
```

**Defaults explained**:
- **`--squash`** — condenses the branch's commits into one on master. Keeps master history linear and easy to revert. Default for this project.
- **`--delete-branch`** — removes the source branch on the remote and (if checked out locally) locally. Always include — keeps branch list clean.

**Alternative strategies** (use only if user specifies):
- `--merge` — preserves all branch commits + adds a merge commit. Use when individual commit history matters for audit/bisection.
- `--rebase` — replays commits onto master without a merge commit. Linear history, but loses branch grouping context.

**After merge, sync local**:
```bash
git checkout master
git pull
# Source branch was auto-deleted on remote. If still present locally:
git branch -D <branch-name>
```

**Quick verify**:
```bash
gh pr view <PR#> --json state,mergedAt,mergeCommit
git log --oneline -3       # confirm squash commit on master
```

If the merge command fails (CI required, conflicts, branch protection), report the error and ask — do not retry with `--force` or `--admin` flags.

## Commit Message Style

Subject line (first line):
- Format: `<type>: <imperative summary>` (under 70 chars)
- `<type>` examples: `docs`, `fix`, `feat`, `cleanup`, `chore`, `build`, `test`
- Imperative mood ("add X", "fix Y") — not past tense

Body:
- Wrap at ~72 chars
- Bullet list of changes per file/area
- Explain *why* if non-obvious (the *what* is in the diff)
- For multi-line messages: always use heredoc, never `-m "..." -m "..."` chaining

Example:
```
docs: update AI git workflow rules (branch-only + PR-for-master)

- CLAUDE.md: replace "AI has NO permission to use git" with new rules
- logs/NEXT_SESSION.md: memory section reflects new git workflow
- .claude/settings.json: allow git/gh; deny force push and hard reset
```

## PR Body Style

Structure:
- **## Summary** — 1~3 bullets, the *why*
- **## Changes** — file/area list, the *what*
- **## Test plan** — checklist (build, sanity, audit)
- Optional **## Notes** — caveats, follow-ups, related work

Add the Claude Code footer:
```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

## When to Ask the User First

| Situation | Action |
|---|---|
| Master merge timing | **Always wait for explicit user instruction.** PR creation is fine, merge is not. |
| Force push / history rewrite | Never. Stop and ask if it seems needed. |
| Deleting a remote branch | Ask first (`gh api -X DELETE` or `git push origin --delete`). |
| Resolving merge conflicts that change behavior | Show the conflict + your proposed resolution, then ask. |
| Unfamiliar branch state (e.g., uncommitted user changes on master) | Investigate, don't overwrite. |

## Allowed Without Asking

- Branch create / checkout / list / delete (local)
- `git status` / `log` / `diff` / `show` (read-only)
- `git add` / `commit` / `push` on **your own branch**
- `git merge` / `rebase` between **non-master branches** (with care for shared history)
- `gh pr create` (creating a PR does not merge)
- `gh pr view` / `list` / `comment`
- `git mv` / `git rm` on tracked files **on your own branch**

## Denied by Settings (Don't Try to Bypass)

- `Bash(git push * --force*)` / `Bash(git push * -f *)`
- `Bash(git reset --hard*)`
- Direct master commits (enforced by policy, not by settings — be vigilant)

## Pre-commit Checklist

- Branch is **not master**?
- Staged list reviewed (`git status --short` or `git diff --cached --stat`)?
- No unintended files (temp logs, secrets, large binaries)?
- `.gitignore` covers any new temp/runtime files generated by this work?
- Commit message follows convention (subject under 70 chars, body wrapped)?

## Recovery from Mistakes

| Mistake | Recovery |
|---|---|
| Committed on master locally (not pushed) | `git branch <new-name>` to save work, then `git reset HEAD~N` (soft) to undo on master. Verify with user. |
| Committed wrong files | `git restore --staged <file>` for staging; new commit to fix; or `git commit --amend` **only if not yet pushed**. |
| Pushed wrong branch | Ask user before any rewrite. Creating a follow-up commit is usually safer than rewriting history. |
| Merge conflict | `git status` to see files, edit to resolve, `git add` resolved files, `git commit`. If unsure, show user and ask. |
