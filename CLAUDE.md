# CLAUDE.md (DetectBase)

This file has two layers:
- **Part A — Working Principles**: how the agent should work (general, adapted from andrej-karpathy-skills).
- **Part B — Project Specification**: what DetectBase is and its concrete rules (project-specific). Part B references Part A where a rule is the concrete application of a principle.

**Tradeoff:** The Working Principles bias toward caution over speed. For trivial tasks, use judgment.

**Language policy:** AI-only artifacts (this CLAUDE.md, `.claude/skills/`, `.claude/agents/`, memory files) are written in English. Anything the user also reads (user-facing responses, commit/PR messages, Korean code comments per the Naming Convention) is Korean.

---

## PART A — Working Principles (process)

### A1. Think Before Coding
**Don't assume. Don't hide confusion. Surface tradeoffs.**
- State assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- Distinguish a *guess* from a *verified fact*. Never present a hypothesis as a conclusion.
- **Effect vs cause:** what you first observe (loss bursts, socket overflow, paused tasks, stuck symptoms) is usually an *effect* — the cause is upstream of it. Don't stop at the first plausible signal; keep asking "what produced this?" until the chain bottoms out at code/config you can name.
- **Assume the library is correct first; suspect your own code/configuration/usage.** External release libraries (GStreamer, librknnrt, FFmpeg, restclient-cpp, sioclient, etc.) are assumed working until proven otherwise. If the library is genuinely the cause, prove it with a minimal reproducer + an upstream issue/MR reference; otherwise the bug lives in our code or our use of the API.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.
- *Concrete application in this project:* the pre-change approval gate in Part B §Work Rules.

### A2. Simplicity First
**Minimum code that solves the problem. Nothing speculative.**
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility"/"configurability" that wasn't requested.
- No error handling for **impossible scenarios** — i.e., internal invariants your own code guarantees (a private function only ever called with valid input; an enum switch with all cases covered; a member guaranteed non-null by construction). This is distinct from **external-boundary defense**, which IS required — see Part B §Design Principles for the boundary definition.
- If you write 200 lines and it could be 50, rewrite it.

### A3. Surgical Changes
**Touch only what you must. Clean up only your own mess.**
- Don't "improve" adjacent code/comments/formatting.
- Don't refactor what isn't broken.
- Remove only the imports/vars/functions YOUR changes orphaned. Don't delete pre-existing dead code unless asked.
- Keep change scope tight — create only the artifacts the task needs. Every changed line should trace to the request. *(Concrete application: branch discipline in Part B §Work Rules / git.)*

**Style precedence (resolves A3 "match existing style" vs the Coding Standard in Part B):**
- **New code** (new files / new functions): follow the Coding Standard & Naming Convention strictly.
- **Editing existing code**: match the surrounding file's existing style, even if it violates the Standard — do NOT mass-reformat or rename to fix pre-existing violations.
- Never introduce a NEW Standard violation; lines you add follow the Standard within the file's structural style.
- If a file's pre-existing style broadly conflicts with the Standard, mention it — don't silently "fix" it as a side effect.

### A4. Goal-Driven Execution
**Define success criteria. Loop until verified.**
- Transform tasks into verifiable goals. Weak criteria ("make it work") require constant clarification; strong criteria let you loop independently.
- For multi-step tasks, state a brief plan with a verify-step each.
- *In DetectBase, "verified" = the pre-merge final program verification gate* (see Part B §Verification): audit 5종 (clang-tidy / cppcheck / ASan / UBSan / TSan) pass + N-hour sanity run + metric baseline, performed before any master/develop merge. Where unit tests don't exist, this gate is the success criterion — substitute it for the generic "write a test" idiom.

**Working if:** fewer unnecessary diff changes, fewer rewrites from overcomplication, clarifying questions come before mistakes.

---

## PART B — DetectBase Project Specification

### Project Goal
General-purpose NPU video analytics base project for Odroid M2 aarch64.
Pipeline: RTSP input → YOLOv5 NPU inference → SORT tracking → boundary intrusion detection → event dispatch → ONVIF metadata → RTSP proxy output

### Build Targets
- Dockerfile.build: aarch64 builder image (protobuf/grpc/restclient-cpp/sioclient source-built in container)
- docker-compose.yml: Odroid M2 service runtime (NPU device + librknnrt mount)

### Work Rules
- Think and reason in English. Always respond in Korean (see Language policy).
- Always read and follow relevant skills in `.claude/skills/` before each task (coding-guidelines, git-workflow, monitoring).
- **Apply A1**: for non-trivial changes, report a plan and wait for approval before editing.
- **Never silently restart the service to make a symptom disappear.** Service restart is an explicit diagnostic action only; state the intent ("재시작해 X 를 확인/적용한다") in the response. Using restart to obscure a problem is forbidden.
- `sed` is on the deny list — use awk/cut/tr for reads; provide complete files for edits.
- No `rm`/`unlink`/`rmdir`. Move with intent: trash → `.deleted/`, rollback/reusable snapshot → `.backup/`. Real `rm` by user only.
- **Git workflow** — AI may use git/gh but never on `master` directly. Work on separate branches; merge to master only via PR on explicit user instruction. Force push / `reset --hard` denied. **Minimize branch proliferation — create only the branches the task needs (instance of A3).**
- **Version-bump 절차 (5/26 정착)** — code/cmake bump 가 같은 commit 에 묶이면 code 와 git tag 의 호환 어긋남 위험. 그래서:
  1. **Bump 는 push 후 별도 commit 으로** — code 변경 commit 은 cmake VERSION 그대로 유지. push 끝낸 다음 cmake 만 올린 별도 commit.
  2. **Develop / master 머지 전**: topic branch HEAD ↔ target branch 의 직전 마지막 commit 비교, 변경 요약을 사용자에게 보고 + **사용자에게 버전 명시적 확인** ("이 머지의 버전은 무엇인가?").
  3. **버전 불일치 시**: 사용자 지정 버전이 commit 의 cmake VERSION 과 다르면 머지 전에 정렬 (새 commit 으로 cmake 만 user-version 으로 올리거나, 기존 commit 의 cmake 를 user-version 으로 수정).
  4. **머지 직후 local placeholder bump**: cmake VERSION 을 (just-merged) + 1 patch 로 별도 commit 후 push (다음 개발 placeholder). 예: 0.1.16 머지 후 → cmake 0.1.17 으로 bump.
  5. **버전 참조 문서 동기**: README / code/README / NEXT_SESSION 등 버전을 직접 인용하는 모든 문서는 머지 시점에 같이 갱신. 코드만 올리고 docs 가 뒤처지면 "cmake vs docs 불일치" 사고 (실제로 v0.1.16 머지 때 발생).
- **Master merge gate (release-grade verification)** — develop → master merge 는 사용자 명시 허가 필수 + 다음 검증 통과:
  - **patch / minor bump**: 모든 audit (5종) 통과 + **3시간 이상 운영 모니터링** (DFPS / RSS / FD / Thread / wd 안정 추세, 후술 §Verification 기준)
  - **major bump**: 모든 audit (5종) 통과 + **각 10시간 이상의 에이징(aging) 모니터링** (장시간 메모리 / FD / Thread leak 추적) + **스트레스 모니터링** (max cam / max load 시 안정성 + DFPS dip 분포 + wd 빈도) + 사용자 명시 허가
  - 사용자 허가 없이 master 가지 않는 게 절대 원칙. AI 가 "검증 완료, 머지하자" 라고 결론 내지 않고 사용자 결정 기다림.

### Coding Standard
- C++17, enforce RAII, guarantee exception safety.
- Explicit ownership via smart pointers (unique_ptr default / shared_ptr only when truly shared / weak_ptr to break cycles).
- const correctness required. Doxygen on all public APIs.
- (Style precedence when editing existing code: see A3.)

### Naming Convention
| Item | Rule | Example |
|------|------|---------|
| Class | PascalCase | DataManager |
| Function | PascalCase | GetData |
| Variable | snake_case | data_count |
| Indent | tab | — |
| Comments | Korean | // 데이터 초기화 |

### Prohibited
- raw new/delete · C-style cast · using namespace std (in headers) · recursion · C++ exceptions (use return values)

### Design Principles
- Explicit lifetime (document shared_ptr ownership).
- Backpressure: every queue has max_size + drop-oldest.
- Graceful degradation: one unit/camera failure must not kill the process; skip + record a metric.
- **Test environment must be stricter than production.** If a failure mode only manifests under test (looping file sources, synchronized loop boundaries, harsh timing, synthetic stress), it is still a real defect — fix the code, do not dismiss with "won't happen in prod." Test is the safety net that catches what live deployments will eventually hit.
- Shutdown order fixed and documented (`// !!! DO NOT REORDER !!!`).
- Observability: every branch/failure/drop is a Prometheus metric; logs are JSON + correlation_id.
- **External-boundary defense (reconciles with A2):** wrap genuinely-fallible external inputs with `catch(...)` / validation. **External boundary = input from outside this process: RTSP stream data, NPU library (librknnrt) calls, network, file IO, third-party libs.** Defense is required here because failure is genuinely possible. Do NOT add defensive code for internal invariants the code already guarantees (that is A2's "impossible scenario").

### Verification (pre-merge final gate)
Before any master/develop merge, run the final program verification:
- **audit 5종**: clang-tidy, cppcheck, ASan, UBSan, TSan.
- **N-hour sanity / monitoring run**: stable DFPS, RSS, no new metric anomalies, cam stuck watch.
- **metric baseline**: q_drop 0, error metrics within accepted bounds.
- **Single-variable intervention for root-cause claims**: for any non-trivial root cause assertion, prefer a controlled A/B experiment — apply the predicted fix, hold everything else, compare matched-duration windows (storm/stuck rate, error counts). Long passive observation alone establishes *correlation*, not *causation*; intervention closes the loop.
- **Patch must be live in the running process**: after any code/build change, verify the patch is actually loaded by the running binary — grep a unique symbol/string in the mapped `.so`, observe a marker log, or confirm container restart picked up the new artifact. "Source is fixed" ≠ "running process is fixed."

This gate is DetectBase's definition of "verified" (A4). Unit tests are not the primary mechanism.

### Master merge gate — bump 종류별 검증 요건

| bump 종류 | audit | 모니터링 | 사용자 허가 |
|---|---|---|---|
| **patch** (0.0.X) | 5종 모두 통과 | **3h+** 운영 안정 추세 | 명시 허가 필수 |
| **minor** (0.X.0) | 5종 모두 통과 | **3h+** 운영 안정 추세 | 명시 허가 필수 |
| **major** (X.0.0) | 5종 모두 통과 | **각 10h+** 에이징 (leak/FD/thread 장시간 추세) + **10h+** 스트레스 (max cam / max load 안정성, DFPS dip 분포, wd 빈도) | 명시 허가 필수 |

- 운영 모니터링 = `logs/monitor.sh <label>` JSONL + DFPS / RSS / FD / threads / wd / reset / eos / disk / docker_cpu/mem 추세 점검
- "사용자 명시 허가" 의 의미: 사용자가 직접 "master merge 진행해라" 또는 동등한 의사 표현. AI 가 검증 완료 후 "이제 머지하자" 라고 단정 X. 사용자 결정 기다림.
- 검증 부족 시 master 가까이 가지 않음 — develop 에 그대로 두고 모니터링 시간 채우기.

### Directory Structure
- code/ source · engines/ NPU engines · bin/ build output · settings/ config · scripts/ service scripts · logs/ active artifacts · .DOCS/ legacy md · .backup/ rollback/reusable snapshots · .deleted/ trash · .claude/ agents+skills

### Document Lifecycle
Active (logs/) → Legacy (.DOCS/, md only) → Trash (.deleted/) or rollback Backup (.backup/). Update references on move.

### Known Issues (must read)
- Proto generated files must be regenerated inside docker with matching protobuf/grpc version.
- Bundled .a files (libcurl/restclient-cpp/sioclient) ABI-incompatible — source-build in docker.
- Header-library version mismatch → std::bad_alloc crash (no message).
- NPU: `insmod rknpu.ko` → `/dev/dri/renderD129` required.
- Memory allocator: jemalloc via LD_PRELOAD; glibc-only patterns (malloc_trim) are no-op under jemalloc.
- GStreamer rtpmanager long-running leak (external lib, ~340 MB/year, accepted; do not "fix" in our code).
- GstRtspClient cam stale/stuck (under investigation): RTP frames stop mid-stream, TCP ESTAB held, no EOS. Confirmed client-side (UDP socket fills to cap, kernel drops rising = server still sending, GStreamer not draining). Stage localization in progress.
