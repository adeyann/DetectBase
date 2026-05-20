# NEXT_SESSION — v0.1.0 release 후 진입점

**최종 갱신**: 2026-05-20 10:30 KST
**현 상태**: cmake VERSION `0.1.6` (develop). v0.1.0 master tagged. PR #9~#16 develop 누적 (audit cleanup, RTSP URL fix, engine dtor UB fix, version sync, README sync, gst-rtsp 진단 trace). cam 659 stuck 1회 발생 (분석 진행 중, [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md)).

---

## 현 상태 (2026-05-20 기준)

### Git
- **master**: `d9ab212` — v0.1.0 tagged
- **develop**: PR #18 (`a2e6bc3`) — PR #9~#18 누적 (cmake 0.1.7)
- 모든 feature 브랜치 정리됨 (no-ff merge commit 정책 + `--delete-branch`)
- Git workflow: no-ff merge commit, cmake VERSION = git tag, develop 머지 patch +1 auto
- Co-Authored-By trailer 금지 (feedback_no_coauthor_trailer.md)

### 운영 (2026-05-20)
- `detectbase_service` Up (PR #16+#17 진단 binary)
- 4 cam × ~29 fps/cam, plateau 정상
- cam 659 1회 stuck (05:42 KST) → restart 후 복구 + 진단 binary 적용. stuck 재발 대기 중.
- Monitor `b97mx4ehw` persistent (30min cycle, T+0 = 10:44)

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

### NEW-1. GstRtsp stale state root cause 추적 (진행 중)
- **상태**: cam 659 stuck 1회 발생 (2026-05-20 05:42 KST), 진단 binary deployed (PR #16, cmake 0.1.5)
- **진단 추가**: bus_message_total / reset_attempt_total / last_frame_age_sec metric + 모든 bus message type log
- **다음 단계**: cam stuck 재발생 까지 monitoring 대기. 발생 시 metric/log 로 가설 좁히기.
- **자동 복구 (watchdog) 금지**: 사용자 정책상 root cause 식별 전 forced restart 안 함.
- 자세한 분석: [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md)

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
