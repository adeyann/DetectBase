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
- `sed` is on the deny list — use awk/cut/tr for reads; provide complete files for edits.
- No `rm`/`unlink`/`rmdir`. Move with intent: trash → `.deleted/`, rollback/reusable snapshot → `.backup/`. Real `rm` by user only.
- **Git workflow** — AI may use git/gh but never on `master` directly. Work on separate branches; merge to master only via PR on explicit user instruction. develop merge is free (self-verify first); auto patch +1 on develop merge (cmake VERSION = git tag), ask user for minor/major. Force push / `reset --hard` denied. **Minimize branch proliferation — create only the branches the task needs (instance of A3).**

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
- Shutdown order fixed and documented (`// !!! DO NOT REORDER !!!`).
- Observability: every branch/failure/drop is a Prometheus metric; logs are JSON + correlation_id.
- **External-boundary defense (reconciles with A2):** wrap genuinely-fallible external inputs with `catch(...)` / validation. **External boundary = input from outside this process: RTSP stream data, NPU library (librknnrt) calls, network, file IO, third-party libs.** Defense is required here because failure is genuinely possible. Do NOT add defensive code for internal invariants the code already guarantees (that is A2's "impossible scenario").

### Verification (pre-merge final gate)
Before any master/develop merge, run the final program verification:
- **audit 5종**: clang-tidy, cppcheck, ASan, UBSan, TSan.
- **N-hour sanity / monitoring run**: stable DFPS, RSS, no new metric anomalies, cam stuck watch.
- **metric baseline**: q_drop 0, error metrics within accepted bounds.
This gate is DetectBase's definition of "verified" (A4). Unit tests are not the primary mechanism.

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
