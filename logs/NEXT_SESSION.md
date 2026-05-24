# NEXT_SESSION — Option A (MPP 통합) 진행 중

**최종 갱신**: 2026-05-25 02:05 KST
**현 상태**: branch `feature/mpp-integration` (HEAD `05f3031`). Option A partial reset 구조 구현/검증 완료 (avdec_h264 유지). mppvideodec swap 시도 → DFPS 116→28 regression 발견, working tree revert.

---

## 🔴 화요일 (2026-05-26) 9시 출근 후 사용자 처리

### 1. local branch 정리 (1줄)
```bash
git branch -D fix/gst-rtpmanager-leak
```

### 2. (선택) MPP regression 조사 진행 여부 결정
- 진행 시: `feature/mpp-integration` 에서 mppvideodec DFPS regression 원인 파악
- 보류 시: 별도 branch 또는 backlog 로 미루고 ThreadProfiler/MPP 외 작업 진행

---

## 🟡 Option A (MPP 통합) 진행 상황 — 2026-05-25 새벽 기록

### 완료 (`feature/mpp-integration` branch)
- **infra** (commit `0a4f3c6`): Dockerfile.build 에 libmpp (JeffyCN/mirrors mpp-dev) +
  gstreamer-rockchip plugin source build 추가. docker-compose.yml 에 `/dev/mpp_service`
  + `/dev/rga` device mount 추가. `detectbase:1.0` image 재빌드 완료. mppvideodec
  plugin gst-inspect 인식 ✓.
- **structure** (commit `05f3031`): GstRtspReceiver 의 BuildPipeline/TeardownPipeline/
  ResetSourceOnly 를 source/decode side 분리 구조로 refactor (~250줄).
  - DECODE-SIDE (Stop 까지 영구): decode_queue → decoder → videoconvert → capsfilter(I420) → appsink
  - SOURCE-SIDE (매 reset destroy/recreate): rtspsrc → depay → parse [+ tee → raw 분기]
  - BOUNDARY: parse.src (또는 tee.src_%u) → decode_queue.sink
  - **검증**: avdec_h264 유지 상태로 EOS cycle 2회 = 8회 PARTIAL reset 정상 작동, duration 4~16ms. DFPS 116 유지.

### 발견된 regression — mppvideodec swap (working tree 만, 미커밋)
- **변경**: `decoder_ = gst_element_factory_make( "mppvideodec", "dec" )` (avdec_h264 → mppvideodec 1줄)
- **결과**: **DFPS 116 → 28 (4× drop)**. 4 cam × 7 FPS/cam (이전 29 FPS/cam).
- **추가 신호**:
  - mpp 초기화 log 에 `mpp_platform: client 12 driver is not ready!`
  - `mpp_soc: open /proc/device-tree/compatible error` (단 컨테이너 안 `/proc/device-tree/compatible` 직접 cat 은 가능 — libmpp 의 open 패턴 문제)
  - 디코딩 자체는 일부 진행 (frames_total 증가), 단 매우 느림 → **CPU fallback 또는 single-core 만 사용** 의심
- **현 working tree**: revert 됨 (`git checkout HEAD -- code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp`). avdec_h264 정상 운영 복귀 (DFPS 117).

### 다음 세션 진입 시 조사 후보 (priority 순)

| 후보 | 의미 |
|---|---|
| **(A) host kernel MPP driver 호환성** | `/proc/devices` 의 `243 mpp_service` major number / kernel module 버전. libmpp 1.0.x 가 기대하는 driver protocol version 와 host kernel 의 mpp driver 매칭 확인. RK3588 BSP 커널 vs upstream 차이? |
| **(B) `client 12 driver is not ready` 정확 원인** | libmpp source 의 client_12 코드 경로 grep → 어떤 ioctl 이 실패하는지 확인. mpp_service device 권한 (currently root:video, container privileged) 가 충분한지. |
| **(C) gstreamer-rockchip plugin caps negotiation** | mppvideodec 의 sink/src caps. h264parse → mppvideodec 사이 stream-format 동등 여부 (`byte-stream` vs `avc`). |
| **(D) mppvideodec property 조사** | gst-inspect-1.0 mppvideodec 의 properties (예: `arm-afbc`, `hwfd` 등 — 일부 off 해야 software fallback 안 함?) |
| **(E) Option A 자체로 가치 인정 후 MPP 보류** | 현 `05f3031` 의 partial reset 구조만 가치 있음. develop 으로 merge 후 mppvideodec 는 별 issue 로 추적. |

### Option A 자체의 가치 (mppvideodec 와 무관)
- ResetSourceOnly 가 partial reset 으로 변경되어, decoder element 가 영구 보존됨.
- avdec_h264 는 software 라 DMA buffer leak 없지만, **rebuild 비용 자체가 감소** (전체 pipeline destroy 안 함).
- 매 reset duration 4~16ms 측정 (이전 단일 desc 방식 대비 ~5~10× 빠름 추정).

---

---

## 현 상태 (2026-05-24 23:50 KST)

### Git
- **master**: v0.1.0 tagged (변동 없음, develop 보다 56 commit 뒤)
- **develop**: HEAD `c023b4e`, cmake **0.1.11**
  - `c023b4e` chore(audit): ASan/TSan 최소 1시간 강제 (leak 조사 부산물, 가치 있음)
  - `da7412e` Merge fix/rtsp-reconnect-storm into develop — cam stuck 2변종 fix + 권한 모델 + 폴더 정리
  - `cb72341` chore: cmake VERSION 0.1.10→0.1.11 + .gitignore
- **삭제됨**: origin `fix/rtsp-reconnect-storm` (머지 후 잔존 무의미), origin `fix/gst-rtpmanager-leak` (잘못된 조사)
- **잔여 local**: `fix/gst-rtpmanager-leak` (위 화요일 항목 참조)
- Git workflow 변동 없음.

### 운영
- `detectbase_service` Up (HEAD 빌드, post-audit restart since 2026-05-24 12:31 KST, 약 11h+)
- 4 cam × DFPS 113~117 안정, watchdog 발동 0, 영구 stuck 0, connect-timeout 0
- Process RSS 660~711 MB stable (cgroup 진짜 RSS, log `resident=` 기준)
- 진단 override (`docker-compose.override.yml.diagbak`)는 `.backup/` 보관

### Audit baseline (audit_20260524_115656)
| | baseline (5/19) | 현재 (5/24) | 변화 |
|---|---|---|---|
| cppcheck | 59 | 59 | 0 |
| clang-tidy | 0 | 0 | 0 |
| TSan | 137 | 141 | +4 (sampling variance) |
| ASan leak | (없음, 5min run) | 1.2 MB / 5min | 신규 측정 — 대부분 init-time, c023b4e 로 1h 최소화 강제 |

### cam stuck — 머지된 2 변종 fix (요약)
1. **변종 A (storm)**: cam offset desync `(cam_id%4)*500ms` + teardown 후 3s+jitter + StartReceiver 후 frame-flow 확인.
2. **변종 B (silent stuck)**: frame-age watchdog (`cv_.wait_for(5s)` wake → 12s 무프레임이면 in-place reset).

**검증 정리**:
- 24.5h+ 자연 운영 clean (Fix A+B)
- 9.5h Fix A revert 실험 (Fix B만): 429 EOS / 0 storm / in-place reset 100% 성공
- 30분 통제 실험 (WATCHDOG_STALE_SEC=3 + on_eos 차단): 4 cam 전부 watchdog **강제 발동 직접 관측** (단일변수 개입 검증)

### 권한 모델 (2026-05-23 전환, 머지 완료)
- deny-first: `Bash(*)` allow + 123 항목 deny (시스템 파괴/사용자/네트워크/패키지/git/docker 위험).
- `./scripts/permission_log.py` → `logs/permission_log.md` (AUTO/APPROVED/DENIED 3분류).

### leak 조사 — 무효 종료 (2026-05-24)
- 시작 이유: ASan 5분 run "1.2 MB / 10418 alloc" → leak 확정으로 해석.
- 잘못된 추적: RSP-thread log 의 `anon=4220MB` 를 RSS 로 해석. 사실은 `/proc/self/maps` anonymous **가상 주소 공간** 합 — jemalloc reserved 영역 포함, 실제 RAM 사용 아님.
- 진실: process resident RSS 9.5h 동안 658→678 MB stable. ASan leak 대부분 init-time (glib/YoloV5/rknn_init 1회), per-reset 96B × 3 수준.
- 결론: **실질 메모리 leak 없음.** branch `fix/gst-rtpmanager-leak` 폐기.
- 부산물 가치 commit `b92dcdc` → develop cherry-pick (audit ASan/TSan 1h 최소 강제, c023b4e).
- 학습 메모: `feedback_verify_metric_definition.md` (memory) — "지표 출처 코드 grep 후 결론, 변수명 추측 금지".
- 산출물 trash: `.deleted/leak_investigation_misguided_20260524/` (CSV/sh/PLAN.md).

---

## 다음 세션 작업 후보 (우선순위 순)

### NEW-2. `correlation_mismatch` 폭증 root cause 추적 (진행 중, 가장 임박)
- **상태**: PR #16+#17 binary 후 mismatch ~70× (0.4 → 27.5/cam/sec). 30분당 +200K stable plateau.
- **핵심 관찰**: 모든 delta = 정확히 10 frame
- **운영 영향**: frame 시차 ~330ms (10 × 33ms) → bbox / tracking / event detection 정확도 영향. q_drop 0.
- **가설**: PR #16 진단 코드 (`last_frame_ns_` atomic store 매 frame + bus message IncrementCounter 매 호출) cache line contention → InferenceThread push 시간 증가 → result_q backlog.
- **다음**:
  - Phase 1: 진단 항목 비활성 실험 (어느 변경이 trigger 인지)
  - Phase 2: per-correlation_id lookup 또는 handler affinity
  - Phase 3: 진단 binary light version
- 자세히: [MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md)

### A. ThreadProfiler module 신규 작성
- 현재: RspProf/InfProf/EvtProf inline struct + 100 cycle 마다 직접 MLOG_INFO + SetGauge
- 목표: 별도 thread 가 모든 stage timing + queue size 일괄 수집
- 구조:
  - PUSH API: `Sample(stage_name, us)` → atomic accumulator
  - PULL API: `RegisterPullSource(name, getter)` 등록 → ThreadProfiler thread 가 N초마다 drain
- 효과: 측정 빈도/필드 변경 시 ThreadProfiler interval 만 수정. 신규 stage 추가는 한 줄.
- 위치: `code/Profile/ThreadProfiler.h+cpp` 신규
- 비용: ~4시간 (구현 + 마이그레이션 + 검증)
- 빌드 branch: `refactor/thread-profiler` (develop fork)

### I. MPP 통합 재시도
- 이전 시도 (2026-05-14~15) 는 매 reconnect ~10MB RSS leak 으로 롤백. 사용자 명령 (2026-05-15) 로 `mppvideodec` + 2-pipeline architecture 전면 제거.
- **현 코드 실제 상태** (정정 — 이전 NEXT_SESSION 기술 오류):
  - pipeline: `rtspsrc → rtph264depay → h264parse → avdec_h264(SW 디코더) → videoconvert → I420 → appsink`. mppvideodec 없음.
  - `ResetSourceOnly()` 는 함수명 무관 **TeardownPipeline + BuildPipeline 전체 파괴/재생성**. element 보존 메커니즘 미구현.
  - `GstRtspClient.cpp` 의 "mppvideodec 보존" 주석 + log message 도 잘못 (MPP 작업 시 정리 예정).
- **재시도 시 실제 작업 범위**:
  1. element 보존 reset 메커니즘 신규 설계/구현 (현재 없음 — BuildPipeline/TeardownPipeline 분리)
  2. pipeline 교체 `avdec_h264` → `mppvideodec` (한 줄)
  3. 매 EOS 시 mppvideodec 인스턴스 보존 → DMA buffer 재할당 회피 가설 검증 (ASan 1h + 12h+ sanity)
  4. NPU offload 효과 측정 (CPU% 비교)
- 참고: `.deleted/gst_attempt_20260515/`, `.DOCS/GSTREAMER_DEEP_REVIEW.md`, `.DOCS/ONVIF_PAYLOADER_DESIGN.md`.
- 작업 branch: `feature/mpp-integration` (develop fork)

### C. Frame ordering 진짜 fix (조건부)
- 방어 카운터 `detectbase_correlation_mismatch_total{cam_id}` 며칠 운영 후 빈도 측정
- 3h sanity (2026-05-19) 결과 mismatch=0. 발생 빈도 0 추정.
- 발생 빈도 > 0 시:
  - per-correlation_id lookup (`cam_result_qs_` → `map<correlation_id, OutputLayerWrapper>`)
  - 또는 handler affinity (cam → 고정 handler, round-robin 포기)

### E. TSan SafeQueue 추적 한계 (~5건)
- structural redesign (lock 범위 확장 또는 atomic guard)
- 운영 영향 0, 보류 가능

### G. DEBUG virtual lines 제거 (v1.0.0 시점)
- 시연용 임시 코드 제거 (위치: README §14)

### ~~F. GStreamer 1.24+ upgrade~~ ✕ 폐기 (2026-05-24, 사용자)
- Ubuntu 22.04 base + librknnrt ABI 위험 + 비용 대비 효과 불분명. watchdog + reconnect 견고화로 cam stuck 클래스 해결.

### ~~NEW-1. GstRtsp stale root cause + Force Reset~~ ✅ 해결 (2026-05-24, cam stuck 2변종 fix 머지)
- 변종 A/B 분리 + fix + 검증 완료. NEW-1 의 force reset 설계는 사실상 frame-age watchdog 로 구현됨.

### ~~leak 조사 (`fix/gst-rtpmanager-leak`)~~ ✕ 무효 (2026-05-24)
- metric 오해. 학습 메모 저장. 산출물 trash. (위 "leak 조사" 항목 참조)

---

## 다음 세션 진입 시 자동 처리

1. `git status` + `git log develop -5` (현 develop 상태)
2. `git branch -a` (잔여 fix/gst-rtpmanager-leak 확인 — 화요일 사용자 처리 가정 시 사라져있어야)
3. 이 NEXT_SESSION.md 읽기
4. 사용자 명령 또는 위 NEW-2 / A / I / C / E / G 중 선택해서 진행

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + 5 디버깅 원칙 + Known Issues |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [logs/AUDIT_REPORT_20260519.md](AUDIT_REPORT_20260519.md) | v0.1.0 audit baseline |
| [logs/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md) | v0.1.0 release 세션 |
| [logs/STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) | 변종 B 추적 |
| [logs/MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md) | NEW-2 분석 |
| [logs/audit_20260524_115656/](audit_20260524_115656/) | 최신 audit |

## Legacy (`.DOCS/`)

| 문서 | 내용 |
|------|------|
| `.DOCS/FORCE_RESET_DESIGN_20260520.md` | 이전 mitigation 설계 (Fix B watchdog 로 대체됨) |
| `.DOCS/HAPPYTIMESOFT_VS_GSTREAMER_20260521.md` | RTSP server 비교 |
| `.DOCS/GSTREAMER_DEEP_REVIEW.md` | GStreamer Phase 1 deep review |
| `.DOCS/GSTREAMER_REWORK_PLAN.md` | GStreamer Phase 1 rework plan |
| `.DOCS/ONVIF_PAYLOADER_DESIGN.md` | GStreamer Phase 2 ONVIF design |
| `.DOCS/SESSION_DFPS_LEAK_HUNT_20260518.md` | dfps leak hunt 세션 |
| `.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md` | 3차 리뷰 완료 baseline |
| `.DOCS/TEST_48H_20260509_LEAK_FOUND.md` | 48h 테스트 leak 발견 |
