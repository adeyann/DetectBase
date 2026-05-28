# NEXT_SESSION

**최종 갱신**: 2026-05-28 KST
**현 develop HEAD**: cmake VERSION = `0.1.23` (이번 머지 후 `0.1.26`). last master tag = `v0.1.18` (2026-05-27).
**작업 중 branch**: `chore/v0.1.26-cycle` (develop 미머지, HEAD `b009fb0`) — 18 commit 누적. 사용자 명시 허가 받은 후 develop 머지 예정.

## 🚨 진행 중 작업 (compact-safe 진입점)

**[5/28] v0.1.26 cycle — UAF fix 검증 완료, develop 머지 직전**:

이 사이클의 핵심 사건:
- **5/27 audit 에서 TSan SEGV 발견** — `OnJitterStatsTimer` (GstRtspReceiver.cpp:287) 의 `g_object_get(jitterbuffer_, ...)` UAF on freed object. baseline (master_logs/v0.1.18/audit_20260527_091456) 와 비교 시 진짜 회귀 = SEGV 1건만 (자체 코드 race 동일, line shift 만).
- **Root cause**: 0f9ae2c (GMainContext per-instance ctx_) 변경 후 `g_source_remove(id)` 가 default global context 에서만 source 찾음 → `ctx_` 안 source 못 찾음 → timer 가 pipeline destroy 후에도 active → freed `jitterbuffer_` access → SEGV. `bus_watch_id_` 도 동일 결함.
- **Fix (4172ac8)**: `guint id` → `GSource* source` 멤버 변환 + `g_source_destroy(source) + g_source_unref(source)` (context 무관, source 자체 작용). GMainContext per-instance 격리 의도 그대로 유지.
- **검증 결과 (5/28 light audit)**:
  - **ASan/UBSan 1h**: leak 1.22 MB / 10,639 alloc — baseline (4h strict) 1.24 MB / 11,515 alloc 대비 거의 동일 (startup+cycle dominated, 시간 비례 X). 자체 코드 leak 0건 유지. 회귀 0.
  - **TSan 1h**: WARNING 158 (baseline 172, -8%), **SEGV 0** ✅ — UAF fix 결정적 검증.
- **branch rename**: `docs/develop-merge-policy-update` → `chore/v0.1.26-cycle` (5/28). 작업 mix 가 docs 가 아닌 fix+refactor+perf+chore 였기 때문 + 정책 자동 발동 misleading 방지.

**머지 직전 점검**:
- cmake 0.1.26 + README/code/README/NEXT_SESSION 동기 commit 포함 (cmake bump README sync 절대 규칙)
- 18 commit 모두 push 완료
- 사용자 명시 허가 필요 (5/27 PR #29 사고 후 default → explicit-approval 전환)

---

## 📋 남은 작업

### 🟢 검증 완료 (이번 사이클)
- [x] audit 5종 회귀 검증 (light): cppcheck 60 / clang-tidy 1W / ASan-UBSan baseline 동일 / **TSan SEGV 0** + WARN 158
- [x] UAF fix (4172ac8) 결정적 동작 확인 (TSan SEGV 0)
- [x] GMainContext per-instance dedicated context 적용 (multi-cam coupling 해소)
- [x] SafeQueue MO-1 (notify_one + notify_all out-of-lock) 적용
- [x] CameraCluster_DETECTOR → SettingManager 인라인화 (1-use 폐기)
- [x] CLOSE-WAIT defensive close 정책 정착
- [x] detectbase.sh audit light/strict 강도 모드 도입
- [x] git workflow rule 강화 (branch cleanup hint + remote state verification)
- [x] skill/memory 역할 분담 정리 (procedural SSOT = skill, personal context = memory)
- [x] branch rename + 파생 branch 잔재 6개 정리

### 🟡 운영 데이터 누적 대기

#### 1. monitor.sh threshold tuning
- 현 기본값: `ALERT_DFPS_LOW_THRESHOLD=100`, `ALERT_DFPS_LOW_STREAK=2`, `ALERT_RSS_MB_THRESHOLD=1100`, `ALERT_WARN_DELTA_PER_CYCLE=500`, `ALERT_WARMUP_CYCLES=4`
- 운영 1-2주 데이터 누적 후 false-positive 분포 확인 → 재검토
- 5/28 현재 monitor `v0.1.26_uaf_fix_postaudit` label 가동 중

#### 2. cam_loss fix path 실효성 검증
- v0.1.18 TeardownPipeline unref-skip 패치 ([GstRtspReceiver.cpp](../code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp)) 가 사후조치 (defensive workaround)
- 11.3h 모니터 동안 fix path 자체가 발화 X — 다음 자연 stuck 시 검증

**Escalation 순서 (stuck 재발 시)**:
1. Step 1 (현재): 자연 stuck 대기 + monitor + fix path 발화 검증
2. Step 2: `GST_DEBUG=2,rtspsrc:5,udpsrc:5,rtpsession:5` env var 추가 후 재실행
3. Step 3: tcpdump packet capture (RTP/RTCP/RTSP)
4. Step 4 (최후): `37dae37` parent commit (happytimesoft 시점) 빌드 + 동일 환경 A/B test

자세한 배경: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md)

#### 3. 24일 storm — accept as baseline
- 정밀 mechanism: INF push mutex contention (40x) + inflight_q drop-oldest → correlation_id mismatch
- 24h 중 3회 (~10분), self-healing, fix 비용 > 효익
- 처리 방침: **accept**. 6+ cam scale-up 시 batch>1 도입 검토 (별도)

---

### 🟠 scale-up 의사결정 후

#### 4. NPU batch_size 수정 (6+ cam scale-up 시)
- [YoloV5_Torch_Onnx_RKNN_NPU.cpp:412](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L412) 잘못된 batch_size 검증
- [L512](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L512) `input.size = rknn_model_w * rknn_model_h * rknn_model_c` 단일 frame 만 할당
- 6+ cam scale-up 시 NPU 천장 도달 시 batch>1 검토 + RKNN 모델 batch>1 재변환 필요 (3가지 동시 fix)

#### 5. SafeThread → ThreadPool 전환
- 현 SafeThread 29건 사용, cam 별 인스턴스 분리 (no pool)
- 카메라 8~16대 확장 계획 있을 시 검토

---

### 🔵 v2.0.0 Multi-engine (~3-4주)

[.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) 참고. MAIA event-driven 패턴, NPU 부담 미미 (~2.4 inference/sec).

- Phase 1 — EngineProfile 확장
- Phase 2 — Search engine ResNet50 RKNN 변환
- Phase 3 — EngineLoadBalancer 다중 engine type routing
- Phase 4 — DETECTOR event-driven Search 경로
- Phase 5 — 운영 메트릭 수집

---

### 🟣 v1.0.0 안정화 후

#### 6. GStreamer 1.20.3 → 1.24+ upgrade
- rtpmanager long-running leak (외부 lib, ~340 MB/year, accepted) 의 fix 가능성 검증
- 비용: 1.5~2시간 + Ubuntu 22.04 → 24.04 base 변경 + librknnrt ABI 호환 위험 + protobuf/grpc source rebuild
- 1.24 changelog 에 명확한 본 케이스 fix 단서 없음 → 효과 불확실

---

## 운영 metric / monitor

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge), 계산: `total_inferences_in_interval / interval`

### canonical monitor
- `logs/monitor.sh <label>` — JSONL, 70+ per-cam fields, 1분 단위 + threshold alerts 7종 (`★storm` / `★err` / `★dfps_low` / `★memory` / `★watchdog` / `★ftc` / `★cam_loss`) + warmup grace 4 cycle
- env override: `ALERT_WARN_DELTA_PER_CYCLE` / `ALERT_DFPS_LOW_THRESHOLD` / `ALERT_DFPS_LOW_STREAK` / `ALERT_RSS_MB_THRESHOLD` / `ALERT_WARMUP_CYCLES`

### audit 강도 모드 (0.1.26 신규)
- `./detectbase.sh audit` (no flag) = **light** default: ASan 60m + TSan 60m (~1h 30min) — develop/내부/branch 검증
- `./detectbase.sh audit --strict` = ASan 240m + TSan 60m (~5h) — **master merge gate 의 audit 5종 = strict 강도 필수**
- `ASAN_DURATION_MIN` / `TSAN_DURATION_SEC` env var override 가능, 강도 모드 default 보다 우선
- `master_logs/v<version>/` baseline 산출물 = strict 강도 결과

### single-instance lock
- `/DetectBase/logs/.detectbase.lock` — `flock(2)` advisory lock, Main.cpp 부팅 시 획득
- 두 번째 instance 시도 시 `[FATAL] another DetectBase instance is running` exit 3

### DEBUG_MODE compile-out (v0.1.20+ PR #28)
Debug 빌드 (`-DCMAKE_BUILD_TYPE=Debug` 또는 audit 5종) 에서만 활성. Release 빌드에선 preprocessor 제거 → 0 runtime cost.

| 영역 | 위치 |
|---|---|
| `DBG_PROF(...)` 매크로 + InfProf/RspProf/EvtProf 100-cycle dump | [RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) |
| jemalloc mallctl 5회 + `/proc/self/maps` 파싱 | RspProf 안 |
| event_detected MLOG_INFO | per event |
| [DFPS] 10초 line | [InferenceCounter.cpp:136](../code/Management/worker/src/InferenceCounter.cpp#L136) |
| GstRtspReceiver debug/stuck trace metric 6개 | BUS_MSG / RESET_ATTEMPT / LAST_FRAME_AGE_SEC / DEPAY_BUFFER / DECODED / RTP_IN |
| GstRtspReceiver jitterbuffer 3개 gauge | JB_PUSHED / JB_LOST / JB_RTX_COUNT |
| DETECTOR [GRPC RECV] + GrpcEventClient [GRPC OK] MLOG_INFO | per call |
| SioHandler ack response MLOG_INFO | per ack |

### DEBUG VIRTUAL LINES (v0.1.19+ PR #28)
- `ServerSetting.debug_virtual_lines_enabled` (default false) toggle
- enable 시 모든 카메라에 schedule 99999 (LineIntrusion) + 99998 (VehicleIntrusion) 강제 주입
- 사용법: [README.md §14](../README.md)

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.26) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + git workflow 정책 (source of truth) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 Search engine 도입 가이드 |
| [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md) | cam_loss / GStreamer stuck 분석 (escalation playbook 포함) |
| [.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md](../.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md) | SafeQueue race deep review (v1.0.0 진입 전 점검, 자체 race 0건 확정) |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [master_logs/v0.1.18/](../master_logs/v0.1.18/) | v0.1.18 archival (audit + monitor JSONL + README) |
