---
name: coding-guidelines
description: Must read before writing or modifying any code. Covers code style, error handling, thread synchronization, and prohibited patterns. Triggered on all code-related tasks including writing, review, debugging, and refactoring.
---

# Project Coding Guidelines

Project-level rules (canonical) are in CLAUDE.md Part B §Coding Standard / §Naming Convention / §Prohibited / §Design Principles. This skill mirrors them and adds **procedural checklists** (pre-work, approval, pre-submit) that are unique to this file.

## First Principle — KISS (Keep It Simple, Stupid)
Avoid unnecessary or excessive code. This is the absolute standard for all code work.

## Build Type Policy (2026-05-28)
- **AI builds only Debug.** `./detectbase.sh compile` defaults to Debug. Use `--debug` if explicit.
- **Release build is user-only.** AI must NOT use `--release` or `CMAKE_BUILD_TYPE=Release`.
- Rationale: diagnostic capability — Debug enables DEBUG_MODE (all log/metric emit, DBG_PROF dumps, jemalloc mallctl, GstRtsp debug/jitter trace). Release is the user's deliberate production deploy decision.
- audit (ASan/UBSan/TSan) forces Debug regardless (existing policy, unchanged).

## Pre-work Checklist
- Re-read the instructions
- Working from actual code, not assumptions

## Approval Required Before Work
Report and wait for approval if any of these apply:
- File structure or class design changes needed
- Modification scope spans 3+ files
- Existing interface (function signature) changes needed

## Code Style

| Item | Rule | Example |
|------|------|---------|
| Class | PascalCase | `DataManager` |
| Function | PascalCase | `GetData` |
| Variable | snake_case | `data_count` |
| Indent | tab | — |
| Comments | Korean | `// 데이터 초기화` |

(No member-variable naming rule. When editing existing code, match the file's existing member style — see CLAUDE.md A3 Style precedence.)

## Error Handling
- Handle errors via return values
- Log before returning on error
- Log format: `[ERROR] function_name : message`
- C++ exceptions are prohibited

## Prohibited Patterns
- Minimize global variables
- No recursion
- No raw new/delete
- No C-style casts
- No using namespace std (in headers)
- Always guard against memory leaks

## Pre-work Scope
- Check both interface definition and implementation of target file
- Check upstream callers of the modification target
- Understand related data structures/class definitions first

## Thread/Synchronization Rules
- Shared resources must be protected with synchronization
- No external function calls while holding a lock
- No nested locks
- Minimize lock scope — lock only where needed
- Thread creation: pthread only

## Pre-submit Checklist
- No omitted or missing code?
- No undeclared variables used?
- No duplicate declarations of existing symbols?
- Compliant with KISS principle?