# NEXT_SESSION — cam stuck fix 검증 + 권한 모델 전환 후 진입점

**최종 갱신**: 2026-05-24 00:10 KST
**현 상태**: branch `fix/rtsp-reconnect-storm` (HEAD `8239fac`, 미머지). cam stuck 2 변종 분석·수정·23h+ 클린 검증. 권한 모델 deny-first 로 전환 (`Bash(*)` allow + deny 69 항목).

---

## 현 상태 (2026-05-24 기준)

### Git
- **master**: v0.1.0 tagged (변동 없음)
- **develop**: cmake 0.1.10 (변동 없음)
- **현재 작업 branch**: `fix/rtsp-reconnect-storm` — 다수 commit 누적, **미머지 (사용자 지시 대기)**.
  - `fffa983` fix: RTSP cam stuck 2변종 해결 — reconnect 견고화 + frame-age watchdog
  - `93c5f9a` chore: cam stuck 진단 마무리 정리
  - `ef38067` / `b87d0fa` chore: 휴지통/백업 폴더 분리 `.deleted_backup` → `.deleted/` + `.backup/` 신설 (+ .gitignore)
  - `029876b` docs: CLAUDE.md 5 디버깅 원칙 (effect/cause, library-first, test-env strict, A/B intervention, patch-live verify)
  - `aa4a0d2` ~ `6db618b` chore: allow list 보강 (cp/docker build/sleep/until/for/time + 변수 prefix 23개)
  - `ace020c` / `d0538e2` chore: permission_log 메커니즘 (`scripts/permission_log.py`)
  - `ba439f9` revert: single-line Bash discipline (취하)
  - `db20481` chore: 권한 모델 전환 — `Bash(*)` allow + `_deny_candidates` 보관
  - `8239fac` chore: `_deny_candidates` 전부 `deny[]` promote (69 항목)
- Git workflow: 변동 없음 (no-ff, no force push, Korean commit msg, no Co-Authored-By).

### 운영 (현재, ~23h+ clean)
- `detectbase_service` Up (patched libRTSP_GST.so 로드, 2026-05-23 01:01 KST 재시작 이후)
- 4 cam × ~116 DFPS, watchdog 발동 0, 영구 stuck 0, connect-timeout 0
- Monitor `bgyvt1hvo` persistent — 30min heartbeat, 의미 있는 이벤트(영구 stuck / watchdog 발동 / storm)에만 보고
- 진단 override (`docker-compose.override.yml.diagbak`)는 `.backup/` 보관 (재사용 가능)

### cam stuck — 2 변종 확정 + 수정
1. **변종 A (reconnect storm)**: 동기화 loop 경계의 너무 빠른 reconnect 가 RTSP 서버 SDP connect timeout 폭주 유발. + `StartReceiver()=true` 의 false-success 가 backoff 무력화 → tight 재시도 storm.
   - **Fix** (`GstRtspClient::ReconnectWorker`):
     - EOS in-place 경로 cam offset desync (`(cam_id%4)*500ms`).
     - error 경로 teardown 후 3s+jitter 지연 (서버 세션 release).
     - StartReceiver 후 frame 흐름 확인 (실제 GetFrameCount>0) — 안 흐르면 backoff 증가.
2. **변종 B (침묵 stuck)**: GStreamer rtpjitterbuffer 내부 race 로 EOS 가 bus 미전파 → on_eos 미발화 → 재연결 미트리거 → 영구 stuck (cam661 식). 우리 reconnect 로직 무관.
   - **Fix** (frame-age watchdog): `cv_.wait_for(5s)` 주기 wake 후 last_frame_ns 체크. 12s 무프레임이면 강제 in-place reset.
3. **검증**: 23h+ 누적 clean. 단 두 변종 자연 재발이 없어 *패치가 막는 장면 직접 관측은 안 됨* — 강한 정황이나 airtight 증명은 아님 (정직).
4. **상위 트리거**: GStreamer `rtpjitterbuffer + 유한 파일 loop RTSP source` 의 엣지 케이스 (잠재적으로 MR 8781 in 1.26.1 로 해결 — 우리는 1.20.3 (Ubuntu 22.04 한계)). 업그레이드 미진행, watchdog 로 graceful degradation.

### 권한 모델 (2026-05-23 전환)
- **deny-first 모델**: `Bash(*)` allow + `permissions.deny[]` 69 항목 (system 파괴 / chown / 패키지 / 방화벽 / 원격 / cron / mount / docker 위험 / git 위험).
- enumerate 패턴은 유지 (문서성 / fallback).
- 분석 도구: `./scripts/permission_log.py` → `logs/permission_log.md` (AUTO/APPROVED/DENIED 3분류).
- 현재: AUTO 99.4% / APPROVED 0 / DENIED 0.6% (fnmatch 기준).
- 메모리: `feedback_trash_dir.md` 최신화 (`.deleted/` + `.backup/` 분리).

### 다음 세션 인계
- **검증 모니터 `bgyvt1hvo`** 계속 가동 (실패 발생 시 즉시 보고). 사용자가 검증 종료 / 머지 결정 대기.
- branch `fix/rtsp-reconnect-storm` 머지 시점은 사용자 지시.
- 진단용 override 는 비활성 (`.backup/docker-compose.override.yml.diagbak`).

(이하 historical content — v0.1.0 release 시점 — 유지)

---

## v0.1.0 시점 (2026-05-20 기준, historical)

### Audit baseline (audit_20260519_222710 — PR #13 H fix 검증)
- cppcheck **59** (이전 63 → PR #9 dead code 제거 -2 + em-dash suppress fix -2)
- clang-tidy **0 ✅** (이전 30 → PR #9 NOLINT 24 + PR #13 진짜 fix)
- ASan/UBSan: startup leak 0 + runtime leak ~1.2MB (GStreamer rtpmanager, 5분 run, 수용)
- TSan: **우리 코드 진짜 race 0 ✅**, 잔여 137 (SIGKILL FP + 외부 lib + 추적 한계)

자세한 결과:
- [AUDIT_REPORT_20260519.md](AUDIT_REPORT_20260519.md) (v0.1.0 baseline, historical)
- [SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md) (v0.1.0 직전 세션)
- [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) (GstRtsp stale 추적)

### PR 누적 (v0.1.0 → develop)
| PR | 변경 | 목적 |
|---|---|---|
| #9 | refactor/audit-cleanup | NOLINT 24 + safe fix 7 + future PR §H 분리 |
| #10 | docs/next-session-reorder | NEXT_SESSION §A 를 §H 뒤로 |
| #11 | docs/next-session-c-reorder | §C 를 §A 뒤로 (mismatch=0 확인 후) |
| #12 | fix/rtsp-url-port | mount `/<id>` + port 555 + ServerSetting wiring |
| #13 | fix/engine-dtor-pure-virtual-call | EngineHandlerBase dtor pure virtual UB 진짜 fix (clang-tidy 30 → 0) |
| #14 | chore/cmake-version-sync-0.1.3 | cmake VERSION 2.2.3 → 0.1.3 (git tag 동기) |
| #15 | docs/readme-version-sync | README v0.1.4 sync + cmake 0.1.4 |
| #16 | debug/gst-rtsp-stale-trace | cam stuck root cause 진단 도구 + cmake 0.1.5 |
| #17 | fix/correlation-mismatch-metric | RegisterCounter 누락 fix + STUCK_ANALYSIS doc + cmake 0.1.6 |
| #18 | docs/post-pr17-sync | README/OPERATIONS/CLAUDE/code 갱신 + cmake 0.1.7 |
| #19 | docs/deep-sync | NEXT_SESSION 현 상태 + PR 누적 표 + cmake 0.1.8 |
| #20 | docs/preserve-mismatch-analysis | MISMATCH_SURGE_ANALYSIS doc 신규 + STUCK_ANALYSIS 부수 발견 추가 + CLAUDE.md Known Issues mismatch surge 추가 + cmake 0.1.9 |

---

## 다음 세션 작업 후보 (우선순위 순)

### B. ~~audit cleanup PR~~ ✅ 완료 (2026-05-19, PR #9 `refactor/audit-cleanup`)
- clang-tidy 30 → 0 (NOLINT 24 + 진짜 fix PR #13)
- cppcheck 63 → 59 (em-dash fix + dead code 제거 + suppress)

### NEW-1. GstRtsp stale state root cause 추적 — cascading stuck 확인 (긴급)
- **상태**: 2026-05-20 17:00~17:45 KST 사이 **cam 660/661/659 cascading stuck**. cam 658 만 alive. DFPS 116 → 29.1.
  - cam 660 stuck since 17:10:33 (34.5분), 661 since 17:16:30 (28.6분), 659 since 17:37:28 (7.6분)
  - 1차 발생 (05:42 KST) → 7시간 무재발 → 17:00~ 폭발적 재발 + cascading
- **진단 확정**: 이전 cam 659 패턴과 동일. EOS reset 정상 → 1~5분 stream 정상 → mid-stream frame stop → 다음 EOS 안 옴. TCP ESTAB 유지. 우리 측 reset trigger 없음.
- **Root cause 가설**: 분기 미확정
  - A. 외부 RTSP server frozen (TCP keepalive 만)
  - B. 우리 측 rtspsrc internal stale (multi-stream race)
  - 가설 분기: tcpdump 또는 force-reset 후 회복 여부
- **Mitigation 설계 완료 (미배포)**: frame-age > 5초 시 force `RequestReconnect`
  - 설계 문서: [FORCE_RESET_DESIGN_20260520.md](FORCE_RESET_DESIGN_20260520.md)
  - 임계값 5초 = 정상 EOS reset 1.6초의 3× margin
  - 검출 위치: RtspDetectorUnit.cpp:1426 dequeue_wait_for timeout 분기
  - 새 metric: `detectbase_force_reset_total{cam_id, reason}`
  - 배포 보류 — 사용자 지시 ("일단 두고", 2026-05-20 17:50 KST)
- **자동 복구 (watchdog) 금지**: 사용자 정책상 root cause 식별 전 service restart 안 함
- 자세한 분석:
  - [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) — 1차 발생 (05:42 KST)
  - [FORCE_RESET_DESIGN_20260520.md](FORCE_RESET_DESIGN_20260520.md) — mitigation 설계
  - [stuck_dump_20260520_171959/](stuck_dump_20260520_171959/) — cam 660 stuck forensic dump
  - [BASELINE_dump_20260520_140935/](BASELINE_dump_20260520_140935/) — 정상 baseline 비교용

### NEW-2. `correlation_mismatch` 폭증 root cause 추적 (진행 중)
- **상태**: PR #16+#17 binary 적용 후 mismatch ~70× 폭증 (0.4/cam/sec → 27.5/cam/sec). 30분당 +200K stable plateau.
- **핵심 관찰**: 모든 mismatch delta = **정확히 10 frame** (variance 0, cross-cam 아님)
- **동시 이상**: prof_RSP ifq/resp 분포 반전 (10K/24K → 33K/0.15K), HWM 1GB peak, prof_INF push 16→41us 증가
- **운영 영향**: frame 시차 ~330ms (10 × 33ms) → bbox / tracking / event detection 정확도 영향. q_drop 0, cam 4/4 OK.
- **Root cause 가설**: PR #16 의 진단 코드 (`last_frame_ns_` atomic store 매 frame + bus message IncrementCounter 매 호출) 의 cache line contention 으로 InferenceThread push 시간 증가 → result_q backlog 10 frame stable 형성.
- **다음 단계**:
  - Phase 1: 진단 항목 비활성 실험 (어느 변경이 trigger 인지 식별)
  - Phase 2: PR #9 §C 옵션 적용 (per-correlation_id lookup 또는 handler affinity)
  - Phase 3: 진단 binary light version 또는 제거 검증
- 자세한 분석: [MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md)

### D. ~~RTSP URL / publish port 정정~~ ✅ 완료 (2026-05-19, `fix/rtsp-url-port`)
- mount path `/cam<id>` → `/<id>`
- default port `8554` → `555` (RtspHandler.h + GstRtspProxyServer.h)
- `ServerSetting.RtspPort` (MVAS dynamic) → `RtspHandler::Setting.proxy_server_port` 실제 연결 (NetworkManager::BuildRtspSetting)
- 외부 viewer URL: `rtsp://<host>:555/<id>` (이전 `rtsp://<host>:8554/cam<id>`)

### E. TSan SafeQueue 추적 한계 (~5건)
- structural redesign (lock 범위 확장 또는 atomic guard)
- 운영 영향 0이라 보류 가능
- v0.1.x cleanup PR 에 포함 가능

### F. GStreamer 1.24+ upgrade (v1.0.0 후)
- rtpmanager runtime leak (~340 MB/year) fix 시도
- Ubuntu 22.04 → 24.04 base + glibc/GCC 동반 업그레이드
- librknnrt ABI 호환 위험 사전 검증 필요
- 비용 ~1.5~2시간 + 위험

### G. DEBUG virtual lines 제거 (v1.0.0 시점)
- 시연용 임시 코드 제거 (위치: README §14)

### H. ~~EngineHandlerBase dtor pure virtual UB fix~~ ✅ 완료 (2026-05-19, `fix/engine-dtor-pure-virtual-call`)
- derived dtor (`~YoloV5_Torch_Onnx_RKNN_NPU`) 에 `if(IsActive()) TerminateEngine();` 명시 호출 추가
- base dtor (`~EngineHandlerBase`) 의 `TerminateEngine()` 호출 제거 → ERROR log 만 (계약: derived 가 호출 책임)
- `EngineHandlerBase.cpp:79` 의 `NOLINTNEXTLINE(clang-analyzer-cplusplus.PureVirtualCall)` 제거 (UB 실제 해결)
- 호출 경로 모두 derived 살아있는 동안 처리 → pure virtual UB + Preprocess race 둘 다 회피

### A. ThreadProfiler module 신규 작성
- 현재: RspProf/InfProf/EvtProf inline struct + 100 cycle 마다 직접 MLOG_INFO + SetGauge
- 목표: 별도 thread 가 모든 stage timing + queue size 일괄 수집
- 구조:
  - **PUSH API**: 각 thread 가 `Sample(stage_name, us)` 호출 → atomic accumulator 누적
  - **PULL API**: queue size / metric 등 외부 source 는 `RegisterPullSource(name, getter)` 등록 → ThreadProfiler thread 가 N초마다 drain
- 효과: 측정 빈도/필드 변경 시 ThreadProfiler interval 만 수정. 신규 stage 추가는 Sample 한 줄. log format 일관.
- 위치: `code/Profile/ThreadProfiler.h+cpp` 신규
- 마이그레이션: RspProf/InfProf/EvtProf → Sample 호출 (~5줄/thread) + RegisterPullSource Init (~15줄)
- 비용: ~4시간 (구현 + 마이그레이션 + 검증)
- 빌드 branch: `refactor/thread-profiler` (develop fork)

### C. Frame ordering 진짜 fix (조건부)
- 방어 카운터 `detectbase_correlation_mismatch_total{cam_id}` 며칠 운영 후 발생 빈도 측정
- 3시간 sanity (2026-05-19) 결과: mismatch=0 일관. 발생 빈도 0 추정.
- 발생 빈도 > 0 시 옵션:
  - per-correlation_id lookup (`cam_result_qs_` → `map<correlation_id, OutputLayerWrapper>`)
  - handler affinity (cam → 고정 handler, round-robin 포기)
- 발생 빈도 0 시 보류

---

## 다음 세션 진입 시 자동 처리

1. `git status` + `git log -3` (현 상태)
2. `git branch -a` (브랜치 정리 상태 — master, develop 만 있어야)
3. 이 NEXT_SESSION.md 읽기
4. [SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md) 의 "v0.1.0 release 마무리" 섹션 (마지막) 읽기
5. 사용자 명령 또는 위 A~G 중 선택해서 진행
   - A (ThreadProfiler) 가 가장 자연스러운 다음 단계
   - B (audit cleanup) 는 짧고 명확한 작업
   - 둘 다 develop fork 으로 작업 → PR 머지 (no-ff)

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (§15 audit baseline 갱신됨, §17 분기 트리거 보류 항목) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + Known issues (rtpmanager leak 추가됨) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [logs/AUDIT_REPORT_20260519.md](AUDIT_REPORT_20260519.md) | audit 결과 + rtpmanager A 결정 |
| [logs/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md) | 이번 세션 전체 진행 (B3/B4 + audit + TSan fix + v0.1.0 release) |
| [logs/NPU_MODEL_PERFORMANCE.md](NPU_MODEL_PERFORMANCE.md) | YOLOv5 s/m/l/x 성능 예측 |

## Legacy (참조 가치 있는 완료 작업, .DOCS/ 로 이동)

| 문서 | 내용 |
|------|------|
| `.DOCS/GSTREAMER_DEEP_REVIEW.md` | GStreamer Phase 1 deep review |
| `.DOCS/GSTREAMER_REWORK_PLAN.md` | GStreamer Phase 1 rework plan |
| `.DOCS/ONVIF_PAYLOADER_DESIGN.md` | GStreamer Phase 2 ONVIF design |
| `.DOCS/PHASE1_CODE_REVIEW.md` | Phase 1 code review |
| `.DOCS/BASICLIBS_AUDIT.md` | BasicLibs 초기 audit |
| `.DOCS/SESSION_DFPS_LEAK_HUNT_20260518.md` | 이전 세션 (dfps leak hunt) |
| `.DOCS/CODE_REVIEW_SUMMARY.md` | 1차 코드리뷰 (legacy) |
| `.DOCS/REVIEW2/`, `.DOCS/REVIEW3/` | 2차, 3차 코드리뷰 |
| `.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md` | 3차 리뷰 완료 baseline |
| `.DOCS/TEST_48H_20260509_LEAK_FOUND.md` | 48h 테스트 leak 발견 |
