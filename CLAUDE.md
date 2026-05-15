# DetectBase

## Project Goal
General-purpose NPU video analytics base project for Odroid M2 aarch64.

Pipeline: RTSP input → YOLOv5 NPU inference → SORT tracking → boundary intrusion detection → event dispatch → ONVIF metadata → RTSP proxy output

## Build Targets
- Dockerfile.build: aarch64 builder image (protobuf/grpc/restclient-cpp/sioclient source-built in container)
- docker-compose.yml: Odroid M2 service runtime (NPU device + librknnrt mount)

## Work Rules
- Think and reason in English. Always respond in Korean.
- Always read and follow relevant skills in `.claude/skills/` before each task. Current skills: `coding-guidelines` (code changes), `git-workflow` (any git/gh operation), `monitoring` (long-running observation / sanity / RSS tracking).
- Report plan and wait for approval before making changes
- **`sed` is on the deny list** — use `awk`/`cut`/`tr` for read-style queries, and provide complete files for edits. Never invoke `sed` directly.
- **No `rm`/`unlink`/`rmdir` direct deletion** — use `mv` to `.deleted_backup/` instead. Actual `rm` only by user.
- **Git workflow** — AI may use git/gh, but **never on `master` directly**. Always work on a separate branch (create as many as needed). Merge to `master` must go through a Pull Request and is executed only when the user explicitly instructs. Merges between non-`master` branches are free. Force push (`--force`/`-f`) and `git reset --hard` are denied. (Defined 2026-05-15)

## Coding Standard
- C++17, enforce RAII, guarantee exception safety
- Explicit ownership via smart pointers (`unique_ptr` / `shared_ptr` / `weak_ptr`)
- const correctness required
- Doxygen comments on all public APIs

### Smart Pointer Selection
- **`unique_ptr<T>`** : Single owner (default choice, ~90% of cases). Lightest overhead.
- **`shared_ptr<T>`** : Multiple owners (only when truly shared). Atomic ref count cost.
- **`weak_ptr<T>`** : Non-owning reference. Breaks cycles, prevents dangling. Use `lock()` to access.

### Smart Pointer Cautions
- **Avoid shared_ptr overuse**: Prefer unique_ptr unless real sharing is required.
- **Break cycles**: A→shared_ptr(B), B→shared_ptr(A) leaks. Use weak_ptr on one side.
- **enable_shared_from_this**: Use when `this` needs a shared_ptr (e.g., GrpcEventServerBase → handler holds weak_ptr).
- **weak_ptr access**: Always `if( auto sp = wp.lock() )`. nullptr if expired.
- **Raw pointer T*** : Only for non-owning short-lived reference within a single function. Add a comment: `// weak ref, owner: X`.

## Design Principles
All project code follows these principles:

- **Explicit lifetime**: Document shared_ptr ownership with comments (e.g., `// server 가 잡음`, `// unit 이 잡음`).
- **Backpressure**: Every queue has `max_size` + drop-oldest policy. Memory does not accumulate even when NPU cannot keep up.
- **Graceful degradation**: Failure of one unit / one camera must not kill the whole process. Skip the failed part and record a metric.
- **Shutdown order verified**: Shutdown sequence is fixed and documented in code (`// !!! DO NOT REORDER !!!`). Prevents UAF.
- **Observability**: Every branch / failure / drop is exposed as a metric (Prometheus). Logs are JSON + correlation_id.
- **External library protection**: Use `catch(...)` to absorb external throws (self-throw stays at 0). Keep `noexcept` signatures consistent.

Details: [README.md §5](README.md#5-설계-원칙)

## Naming Convention
| Item | Rule | Example |
|------|------|---------|
| Class | PascalCase | `DataManager` |
| Function | PascalCase | `GetData` |
| Variable | snake_case | `data_count` |
| Indent | tab | — |
| Comments | Korean | `// 데이터 초기화` |

## Prohibited
- raw new/delete
- C-style cast
- using namespace std (in headers)
- Recursion
- C++ exceptions (use return values for error handling)

## Directory Structure
- code/: source code
- engines/: NPU inference engines
- bin/: build output (cmake/ninja artifacts; regenerated on build)
- settings/: configuration files (EngineSettings.json, NetworkSettings.json)
- scripts/: service scripts (run_48h_test.sh, logrotate.detectbase)
- logs/: active work artifacts — analysis (.md), runtime logs (.log), in-progress tests
- .DOCS/: legacy .md only — completed reviews, historical baselines (no further updates; retained for reference)
- .deleted_backup/: trash — .log files, raw data, deprecated artifacts. `mv` into here is the AI-allowed "delete"; actual `rm` by user only
- .claude/: AI agent definitions + skills (build-runner, cpp-debugger/explorer/reviewer, doc-writer, coding-guidelines skill)

## Document Lifecycle Policy
- **Active → Legacy → Trash**: When an .md artifact is no longer updated but still has reference value, move it from logs/ to .DOCS/. When it has no further use, move from .DOCS/ to .deleted_backup/.
- **.md only in .DOCS/**: Other formats (.log, .txt, raw data) belong in .deleted_backup/, not .DOCS/.
- **Update references on move**: After moving a document, grep for its old path and fix every link. Verify no broken refs remain.

## Known Issues (must read)
- Proto generated files must be regenerated inside docker with matching protobuf/grpc version
- Bundled .a files (libcurl, librestclient-cpp, libsioclient) are ABI incompatible — source-build in docker
- Header-library version mismatch causes std::bad_alloc crash (no error message)
- NPU: `insmod rknpu.ko` → `/dev/dri/renderD129` required
- docker-compose: build and service must share the same image name
- Memory allocator: jemalloc via `LD_PRELOAD` (docker-compose.yml). glibc-only patterns like `malloc_trim(0)` (W-14) are effectively no-op under jemalloc — page reclamation is handled by `background_thread:true` (MALLOC_CONF). Don't add new glibc-malloc-tuning patches.
