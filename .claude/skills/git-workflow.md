---
name: git-workflow
description: Must read before any git/gh operation. Defines AI's allowed git usage in this project — branch-only work, PR-for-master, commit message style, and forbidden destructive operations. Triggered on all git/gh tasks including commit, push, branch, merge, rebase, PR creation.
---

# Git Workflow for AI

## First Principle — Never touch master directly; develop is the integration gate
**AI must never commit, push, or merge directly on `master`. AI also does not commit directly to `develop` — develop merges happen via PR from a dedicated branch.**
- **`develop`** is the permanent integration branch (Git Flow variant). Feature/fix/docs branches fork from `develop` and merge back to `develop` via PR.
- **develop merge is free** (no user approval needed for feature → develop) — self-verify first (build + sanity). cmake VERSION bump is **NOT** applied during the develop merge of a feature; it is handled separately under the bump procedure below.
- **master** changes only via PR from `develop`, executed only when the user explicitly instructs.

## Hard Rules

| Rule | Detail |
|---|---|
| **Branch-only work** | Always `git checkout -b <branch>` (fork from `develop`) before any modification. AI never commits directly to `develop` or `master`. Create only the branches the task needs — avoid branch proliferation (CLAUDE.md A3). |
| **develop merge via PR is free** | `gh pr create --base develop --head <branch>` then `gh pr merge --merge --delete-branch` — without asking, after self-verification. no-ff merge commit (project default). |
| **PR for master merge — user-instructed only** | `gh pr create --base master --head develop` proposes a release. `gh pr merge` (to master) only when user explicitly says so. Never `git checkout master && git merge` directly. |
| **User-explicit approval for master merge** | `gh pr merge` (to master) only runs when the user says so ("머지해라", "merge it" 등). Do not infer or anticipate approval. |
| **No force push** | `git push --force` / `git push -f` are denied by settings. Never bypass. |
| **No hard reset** | `git reset --hard` is denied. Use `git reset` (mixed) or `git restore` instead. |
| **No history rewrite on shared branches** | Don't rebase or amend commits that are already pushed to a shared branch. |

## Branch Naming Convention

(Mirrors `feedback-git-workflow` memory.)

| Prefix | Use case | Example |
|---|---|---|
| `feature/` | New functionality | `feature/gstreamer-integration` |
| `fix/` | Bug fix | `fix/rtsp-reconnect-leak` |
| `hotfix/` | Production urgent (still via develop, no direct master) | `hotfix/auth-token-expiry` |
| `refactor/` | Behavior-preserving refactor | `refactor/safequeue-cleanup` |
| `perf/` | Performance improvement | `perf/dfps-pipeline` |
| `chore/` | Tooling, deps, CI, policy/doc-only | `chore/bump-grpc-version` |
| `docs/` | Documentation only (when not foldable into in-flight PR) | `docs/update-git-rules` |
| `cleanup/` | Dead code, refactor (legacy alias for refactor/) | `cleanup/remove-w14-malloc-trim` |
| `test/` | Tests only | `test/sanitizer-suite` |
| `experiment/` | Experimental / WIP | `experiment/wd-baseline-rollback` |

Use kebab-case after the prefix. Keep names short but descriptive.

**Sub-branch depth**: 2 recommended, 3~4 allowed if needed. Naming: `<parent-prefix>/<parent-name>-<sub-id>` (e.g., `feature/dfps-async-1`).

## Branch Flow (Git Flow variant)

```
master (last line of defense, user-approved merges only, version tag)
   ↑ no-ff merge commit via PR (from develop only, on explicit user instruction)
develop (permanent branch, kept even when identical to master)
   ↑ no-ff merge commit via PR (from any work branch)
feature/a · fix/a · chore/a · ... (work units, forked from develop)
   ├── feature/a-1 (sub-task, depth 2 recommended)
   └── feature/a-2
```

Only `develop` merges into `master`. Other branches never merge to master directly. Hotfixes also go through develop.

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

# 6. PR to develop (free) — or PR to master (user discretion)
#    develop merge: gh pr create --base develop --head <prefix>/<topic>  → gh pr merge --merge --delete-branch
#                   (AI does NOT commit/merge directly on develop — always via PR)
#    master release: gh pr create --base master --head develop  → merge only on user instruction
gh pr create --base develop --head <prefix>/<topic> \
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

## cmake VERSION Bump Procedure (2026-05-26 — supersedes the "bump in the same PR" rule)

The earlier rule "AI applies the bump in the PR being merged" is **retired**. Bundling code+cmake in one commit makes the commit ambiguous about which version it represents.

**5-step procedure**:
1. **Topic-branch work**: edit code. Leave cmake VERSION unchanged.
2. **Push topic branch**: commit + push code change only. No cmake bump.
3. **Pre-merge user confirmation**: compare topic-branch HEAD against the target branch's most recent commit, summarize the change to the user, and **explicitly ask the user for the version of this merge** ("what version should this merge be?").
4. **Reconcile if the user-specified version differs from the commit's cmake**: align before merging — either add a new commit that only bumps cmake to the user-version, or modify the existing commit to set cmake to the user-version, push, then merge.
5. **Post-merge local placeholder bump**: bump cmake VERSION to (just-merged) + 1 patch as a **separate commit**, then push. This is the placeholder for the next dev cycle (e.g., after merging 0.1.16 → bump to 0.1.17).

**Doc-sync absolute rule (2026-05-27)** — every cmake VERSION change (bump up OR adjust down) must update README.md root `Version`, code/README.md verification-state cmake reference, and logs/NEXT_SESSION.md cmake reference **in the same commit**. Solo cmake bump is forbidden. Pre-commit grep: `grep -nE '0\.[0-9]+\.[0-9]+|VERSION|cmake' README.md code/README.md logs/NEXT_SESSION.md`.

**Pre-push docs check (2026-05-26 absolute rule)** — at **every commit push** (not just merges), sweep all version/status-referencing docs (README / code/README / NEXT_SESSION / OPERATIONS / .DOCS/) and align them with the commit. If drift is found after push, fix it in the very next commit. Check before push is the principle.

## master_logs/v<version>/ Archival (2026-05-27 — required before master merge)

Master merge verification artifacts must be archived in develop tree before master merge for traceability/audit.

**Why**: `logs/audit_*/` is gitignored, so audit results live outside git. Archiving them under `master_logs/v<version>/` (root, NOT gitignored) puts the proof of release inside the tree master will own.

**Procedure**:
1. Create `master_logs/v<version>/` at repo root.
2. Move (not copy) the audit dir `logs/audit_<stamp>/`, the monitoring JSONL covering the merge window, and a README.md summarizing rationale.
3. Land on develop via a **dedicated chore branch + PR → develop merge** (AI does not commit directly to develop). This commit **must NOT bump cmake VERSION** — its code state must equal the version being merged.
4. After this lands on develop, perform `develop → master --no-ff merge` (only on explicit user instruction). master's tree now owns `master_logs/v<version>/`.

**Master merge verification gate (must pass before user grants approval)** — Mirrors CLAUDE.md §Master merge gate (single source of truth) + `feedback-git-workflow` memory:
- audit 5종 (clang-tidy / cppcheck / ASan / UBSan / TSan) all PASS — baseline compared
- monitoring run: **patch/minor = 3h+** stable trend (DFPS / RSS / FD / Threads / wd) / **major = 10h+ aging + 10h+ stress** (max cam / max load, DFPS dip distribution, wd frequency)
- Explicit user approval — AI never concludes "verification done, let's merge"; waits for the user.

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
git checkout master           # or develop, depending on which was the merge base
git pull
# Source branch was auto-deleted on remote (via --delete-branch).
# If still present locally, use safe -d (never -D):
git branch -d <branch-name>
```
Use `-d` (safe, fails if unmerged). `-D` (force) is denied — if `-d` fails, the branch has un-merged commits and needs investigation, not force-deletion.

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

- Branch create / checkout / list / delete (local, using `git branch -d` safe-delete only)
- `git status` / `log` / `diff` / `show` (read-only)
- `git add` / `commit` / `push` on **your own (non-develop, non-master) branch**
- `git merge` / `rebase` between **non-develop, non-master branches** only (e.g., feature/a ← feature/a-1). **Never** `git merge` directly into `develop` or `master` — always via PR.
- `gh pr create` (creating a PR does not merge)
- `gh pr merge --merge --delete-branch` for **develop merges** (after self-verification). For master merges only on explicit user instruction.
- `gh pr view` / `list` / `comment`
- `git mv` / `git rm` on tracked files **on your own branch**

## Denied by Settings (Don't Try to Bypass)

- `Bash(git push * --force*)` / `Bash(git push * -f *)` — force push
- `Bash(git reset --hard*)` — hard reset
- `Bash(git branch -D *)` — force branch delete (use `-d` safe)
- `Bash(git clean -f*)` — force clean
- `Bash(git config *)` — git config write
- `Bash(git filter-branch*)` / `Bash(git filter-repo*)` — history rewrite
- `Bash(git rm *)` — git rm (use trash dir [[feedback-trash-dir]])
- `Bash(git tag -d *)` — tag delete
- `Bash(git push * --delete*)` / `Bash(git push origin --delete*)` — remote branch delete via push (use `gh pr merge --delete-branch` instead)
- `Bash(git stash drop *)` / `Bash(git stash clear)` — stash purge
- `Bash(git worktree remove *)` — worktree remove
- `Bash(git submodule deinit *)` — submodule deinit
- Direct master commits (enforced by policy, not by settings — be vigilant)
- Also: `rm`, `sudo`, `sed`, `docker rm`, `docker kill`, system-destructive (`apt`, `mkfs`, `chown`, etc.) — covered by other memory entries.

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
