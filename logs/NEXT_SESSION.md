# NEXT_SESSION — v0.1.0 release 완료 후 진입점

**최종 갱신**: 2026-05-19 16:00 KST
**현 상태**: `v0.1.0` released. master/develop 동기화 완료. 다음 작업 후보 = ThreadProfiler module 또는 audit cleanup PR.

---

## 현 상태 (2026-05-19 기준)

### Git
- **master**: `d9ab212` — v0.1.0 tagged
- **develop**: `90eee99` — master 와 동기화 (no-ff merge commit `merge: master v0.1.0 sync to develop`)
- 모든 feature 브랜치 정리됨 (squash merge 후 자동 삭제)
- Git workflow 변경: **squash merge → no-ff merge commit** (memory rule 갱신, feedback_git_workflow.md)
- Co-Authored-By trailer 제거 (memory rule feedback_no_coauthor_trailer.md)

### 운영
- `detectbase_service` 정상 가동 중
- 4 cam × ~29 fps/cam, RSS 602~657 MB ±55 MB plateau, q_drop 0
- jemalloc 활성 (background_thread:true)

### Audit baseline (audit_20260519_145745)
- cppcheck **63** (false positive 20 + Profiler 자연정리 9 + needs-review 11)
- clang-tidy **30** (needs-review 24 + 누락 narrowing 4 + exception-escape FP 2)
- ASan/UBSan: startup leak 0 + runtime leak 1 (GStreamer rtpmanager, 수용)
- TSan: **우리 코드 진짜 race 0 ✅**, 잔여 139 (SIGKILL FP + 외부 lib + 추적 한계)

자세한 결과: [AUDIT_REPORT_20260519.md](AUDIT_REPORT_20260519.md), [SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md).

---

## 다음 세션 작업 후보 (우선순위 순)

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

### B. audit cleanup PR (refactor/audit-cleanup)
- v0.1.0 후 별도 PR 로 보류한 needs-review + 누락 항목:
  - clang-tidy needs-review 24:
    - `bugprone-easily-swappable-parameters` 16 (struct wrapper 또는 named-arg 패턴)
    - `bugprone-branch-clone` 4 (의도 vs 버그 검토)
    - `bugprone-string-literal-with-embedded-nul` 1 (RtspDetectorUnit:560)
    - 3 기타
  - clang-tidy 누락 4: YoloV5:50/51/59/60 의 `r_w * img.rows` int→float 명시 cast
  - clang-tidy false positive 2: SettingData:353/363 base ctor exception-escape (NOLINT 또는 base noexcept)
  - cppcheck needs-review 11: useStlAlgorithm 5 + unreadVariable 4 + unusedStructMember 2
- 비용: ~3~4시간
- 빌드 branch: `refactor/audit-cleanup`

### C. Frame ordering 진짜 fix (조건부)
- 방어 카운터 `detectbase_correlation_mismatch_total{cam_id}` 며칠 운영 후 발생 빈도 측정
- 발생 빈도 > 0 시 옵션:
  - per-correlation_id lookup (`cam_result_qs_` → `map<correlation_id, OutputLayerWrapper>`)
  - handler affinity (cam → 고정 handler, round-robin 포기)
- 발생 빈도 0 시 보류 가능

### D. MAIA RTSP URL 정정
- 별도 PR: port 555 + mount `/<id>` (현재 `/cam<id>`)
- NetworkSettings.json 의 `RTSP_Proxy_Port` 추가
- docker-compose.yml port 매핑 검토

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

### H. EngineHandlerBase dtor pure virtual UB fix
- 위치: `code/Engine/EngineBase/src/EngineHandlerBase.cpp:79`
- 현 상태: NOLINT 만 적용 (audit cleanup PR `refactor/audit-cleanup` 2026-05-19)
- 진짜 fix: derived dtor (`~YoloV5_Torch_Onnx_RKNN_NPU`) 에 명시 호출 추가 + base fallback 단순화
  ```cpp
  ~YoloV5_Torch_Onnx_RKNN_NPU() override {
      if( IsActive() ) TerminateEngine();
  }
  ```
- 위험: abnormal shutdown 시 `~EngineHandlerBase` fallback path 진입 시 pure virtual 호출 + Preprocess race 두 가지 UB
- normal path: `DETECTOR.cpp:410` 명시 `TerminateEngine()` 호출 → fallback path 진입 0 (실 운영 안전)
- 비용: ~1시간 (derived dtor 1개 추가 + base 단순화 + 검증)
- 빌드 branch: `fix/engine-dtor-pure-virtual-call`

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
