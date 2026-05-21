---
name: git-workflow
description: Must read before any git/gh operation. Defines AI's allowed git usage in this project — branch-only work, PR-for-master, commit message style, and forbidden destructive operations. Triggered on all git/gh tasks including commit, push, branch, merge, rebase, PR creation.
---

# Git Workflow for AI

## First Principle — Never touch master directly; develop is the integration gate
**AI must never commit, push, or merge directly on `master`.** All AI work happens on a separate branch.
- **`develop`** is the permanent integration branch (Git Flow variant). Feature/fix/docs branches fork from `develop` and merge back to `develop`.
- **develop merge is free** (no user approval needed) — self-verify first (build + sanity). On each develop merge, bump cmake VERSION patch +1 in the same PR (cmake VERSION = git tag; ask user for minor/major). Do not create a git tag.
- **master** changes only via Pull Request from `develop`, executed only when the user explicitly instructs.

## Hard Rules

| Rule | Detail |
|---|---|
| **Branch-only work** | Always `git checkout -b <branch>` (fork from `develop`) before any modification. Create only the branches the task needs — avoid branch proliferation (CLAUDE.md A3). |
| **develop merge is free** | Merge feature/fix/docs branches into `develop` without asking, after self-verification. Use a no-ff merge commit (see below). |
| **PR for master merge only** | `gh pr create --base master --head develop` proposes a release to master. Never `git checkout master && git merge` directly. |
| **User-explicit approval for master merge** | `gh pr merge` (to master) only runs when the user says so ("머지해라", "merge it" 등). Do not infer or anticipate approval. |
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
# 1. Start: branch from develop (the integration base)
git checkout develop
git pull                           # only if develop moved
git checkout -b <prefix>/<topic>

# 2. Work: edit, build, test
# ... (edit files, run build/sanity) ...

# 3. Stage: be specific — never blanket `git add .` if temp files might leak
git status --short
git add <specific files>
git status --short                 # verify staged list

# 4. Commit: heredoc for multi-line (한국어)
git commit -m "$(cat <<'EOF'
<type>: <한국어 요약, 70자 이내>

<본문: 무엇을 왜 바꿨는지, ~72자 줄바꿈, 한국어>

<선택: 참조 footer, 예 "Related: #issue">
EOF
)"

# 5. Push: -u on first push of the branch
git push -u origin <prefix>/<topic>

# 6. Merge to develop (free, no-ff) — or PR to master (user discretion)
#    develop merge:  git checkout develop && git merge --no-ff <prefix>/<topic>
#    master release: gh pr create --base master --head develop  (merge only on user instruction)
gh pr create --base master --head develop \
    --title "<type>: <한국어 요약>" \
    --body "$(cat <<'EOF'
## 요약
<1-3 bullet — 왜>

## 변경 사항
- 파일: 무엇이 바뀌었나
- ...

## 테스트 계획
- [x] build PASS
- [x] sanity (DFPS, ERROR 0)
- [ ] (기타 검증)
EOF
)"
```

## Master Merge Execution (user-instructed)

When the user explicitly says to merge a PR, use this default:

```bash
gh pr merge <PR#> --merge --delete-branch
```

**Defaults explained**:
- **`--merge`** — no-ff merge commit. Preserves the branch's commits and records an explicit merge point. **This is the project default (squash is no longer used).**
- **`--delete-branch`** — removes the source branch on the remote and (if checked out locally) locally. Always include — keeps branch list clean.

**Do not use `--squash`** (retired 2026-05-19). `--rebase` only if the user explicitly asks.

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
git log --oneline -3       # confirm no-ff merge commit on master
```

If the merge command fails (CI required, conflicts, branch protection), report the error and ask — do not retry with `--force` or `--admin` flags.

## Commit Message Style

**Language: commit messages are written in Korean** (subject summary + body). The `<type>` prefix stays as a structural tag (docs/fix/feat/...). User reads commits in the GitHub UI — Korean is mandatory (see CLAUDE.md Language policy / memory feedback-language).

Subject line (first line):
- Format: `<type>: <한국어 요약>` (70자 이내)
- `<type>` 종류: `docs`, `fix`, `feat`, `cleanup`, `chore`, `build`, `test`, `refactor`, `perf`
- 명령형/간결체 ("추가", "수정") — 과거형 지양

Body:
- ~72자 줄바꿈, 한국어
- 파일/영역별 변경 bullet
- *왜* 가 비자명하면 설명 (*무엇* 은 diff 에 있음)
- 여러 줄이면 항상 heredoc 사용, `-m "..." -m "..."` chaining 금지

Example:
```
docs: AI git workflow 규칙 갱신 (develop gate + no-ff merge)

- CLAUDE.md: master 직접 작업 금지 + develop 통합 gate 명시
- .claude/skills/git-workflow.md: squash 폐기, no-ff merge 기본
- .claude/settings.json: git/gh 허용; force push / hard reset deny
```

## PR Body Style

Structure (Korean):
- **## 요약** — 1~3 bullet, *왜*
- **## 변경 사항** — 파일/영역 목록, *무엇*
- **## 테스트 계획** — checklist (build, sanity, audit)
- Optional **## 비고** — caveat, follow-up, 관련 작업

Do NOT add any Claude/Claude Code footer or `Co-Authored-By` trailer to commits or PR bodies (user policy).

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
