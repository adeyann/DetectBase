# NEXT_SESSION — v0.1.19 작업 진입

**최종 갱신**: 2026-05-27 KST
**현 develop HEAD**: cmake VERSION = `0.1.22` (작업 D carry). last master tag = `v0.1.18` (2026-05-27, PR #25). 마지막 push commit cmake = `0.1.21` (작업 C — placeholder dead code 제거).
**진입 branch**: `cleanup/debug-virtual-lines` (develop fork).

---

## 🎯 v0.1.19 작업 계획

### 1. DEBUG VIRTUAL LINES → `config.json` toggle 전환

**현 상태**:
- `AddDebugVirtualLines_REMOVABLE()` ([RtspDetectorUnit.cpp:261-302](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L261)) — 무조건 호출 ([L453](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L453) init, [L477](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L477) settings callback)
- 결과: 모든 카메라에 schedule 99999 (LineIntrusion 사람) + 99998 (VehicleIntrusion 차량) 강제 주입 — 가로 3 + 세로 3 = 6개 line, 1초 cooldown, 24h/7day 활성

**변경**:
- `ServerSettingData` 에 `debug_virtual_lines_enabled: bool` 추가 (default **false**)
- config.json schema 갱신 (`server.debug_virtual_lines_enabled` 또는 `server.debug.virtual_lines`)
- `SettingManager` load 경로 갱신
- 호출 2곳 (L453 / L477) 조건부 호출로
- 함수 이름 `_REMOVABLE` 접미사 제거 → `AddDebugVirtualLines()` (영구 feature)
- enable 시 boot/callback 시점 INFO log 한 줄 (운영 가시화)

**고려사항**:
- v0.1.19 에선 **boot-time 토글만** 지원. runtime 동적 toggle (enable→disable 시 99999/99998 정리) 은 별도 작업.
- 사용자 schedule 과의 우선순위: 그대로 (사용자 + 99999/99998 둘 다 활성).

**위치**:
- 코드: [RtspDetectorUnit.cpp:253-303](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L253) + 호출 L453/L477
- ServerSetting 정의: `code/BasicLibs/core/types/include/MgenTypes.h` 또는 SettingManager 헤더
- config schema: `settings/config.json`
- 문서: [README.md §14](../README.md) — 제거 절차 → toggle 사용법으로 재작성

---

### 2. 디버깅 로그 / 메트릭 → DEBUG_MODE compile-out (production 비활성화)

**매커니즘**: [code/CMakeLists.txt:55](../code/CMakeLists.txt#L55) `add_compile_definitions( DEBUG_MODE )` 이 Debug 빌드에서만 정의됨. Release (`DEBUG_MODE` off) 에선 `#ifdef DEBUG_MODE ... #endif` 블록 전체가 preprocessor 단계에서 제거 → 0 runtime cost.

**Compile-out 대상**:

| # | 위치 | 내용 | Release 효과 |
|---|---|---|---|
| 2a | [RtspDetectorUnit.cpp:930-1282](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L930) | `InfProf` struct + 측정 (`t_dq_set` 등) + 100-cycle MLOG_INFO dump | log 1줄/3sec/cam 제거 + 측정 cost 제거 |
| 2b | [RtspDetectorUnit.cpp:1327-1796](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1327) | `RspProf` struct + 측정 + 100-cycle MLOG_INFO dump (1300B/줄) + jemalloc mallctl 5회 + `/proc/self/maps` 파싱 | log 큰 폭 감소 + **CPU 5-50ms/3sec 회수** (가장 큰 숨은 비용) |
| 2c | [RtspDetectorUnit.cpp:1836-2055](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1836) | `EvtProf` struct + 측정 + 100-cycle MLOG_INFO dump | log + 측정 cost 제거 |
| 2d | [RtspDetectorUnit.cpp:1080](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1080) | `if(false)` 안의 placeholder MLOG_INFO | dead code 즉시 삭제 (DEBUG_MODE 와 무관) |

**부수효과**:
- cppcheck `t_*_set` `knownConditionTrueFalse` 9건 자연 정리
- Release log volume **40-50% 감소** (~6-8 KB/sec → ~3-4 KB/sec, 10cam×30fps 가정)
- Release RSP-thread CPU **약 8-18 ms/sec 회수**
- Release binary 크기 미세 감소

**구현 방식**: file-local 매크로 1개 (`DBG_PROF(...)` — `RtspDetectorUnit.cpp` 안에서만 사용) + 큰 블록 (struct 정의 / lambda / 100-cycle dump) 은 `#ifdef DEBUG_MODE` 직접 wrap. 측정 라인 50+ 곳 ifdef 만으로는 노이즈 과다 → 1-매크로 file-local abstraction 정당 (A2 의 "single-use code 회피" 와 충돌 X — file-internal 50+ 사용처). 다른 파일에는 매크로 노출 X.

---

### 3. 사용자 결정 필요

| 항목 | 위치 | 분류 후보 | 권장 |
|---|---|---|---|
| `event_detected type=%s cam=%d count=%zu` MLOG_INFO | [RtspDetectorUnit.cpp:1931](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1931) | (a) 운영 보존 / (b) DEBUG_MODE compile-out / (c) MLOG_DEBUG 강등 | (b) 또는 (c) |
| `[DFPS] %4.1f FPS/cam ... (TotalDFPS: ...)` 10초 MLOG_INFO | [InferenceCounter.cpp:136](../code/Management/worker/src/InferenceCounter.cpp#L136) | (a) 운영 보존 / (b) DEBUG_MODE compile-out (Prometheus 와 중복) | (b) 권장 |

---

### 4. 작업 순서

1. **#1 (config toggle)** — ServerSettingData 변경 + 호출 2곳 + schema
2. **#3 (사용자 결정)** — event_detected / DFPS 분류
3. **#2 (DEBUG_MODE compile-out)** — #3 결과 반영하여 한 commit
4. **build 검증** — Release + Debug 양쪽 빌드 + sanity (DFPS 회귀 X, log volume 감소)
5. **README §14 + code/README.md 갱신** — config toggle 사용법 + DEBUG_MODE gating 설명
6. **PR → develop** — cmake 0.1.19 bump + 위 변경 일괄 흡수
7. master merge 는 별도 (audit 5종 + 3h+ 모니터링 후 사용자 결정)

### 5. 정책 메모

- cmake 0.1.19 bump 는 **working dir 만 (commit X)** 으로 본 branch 진입 시점에 적용됨 (CLAUDE.md §Version-bump 절차 step 3). 다음 code commit 에 자연 흡수.
- 본 branch (`cleanup/debug-virtual-lines`) commit 들은 별도 PR 으로 develop 머지.

---

## 📋 미해결 latent issues

### cam_loss fix path 미발화 — 실효성 동적 검증 대기
v0.1.18 TeardownPipeline unref-skip 패치 ([GstRtspReceiver.cpp:314-340](../code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp#L314)) 가 사후조치 (defensive workaround). stream stop / GStreamer thread join 실패의 **진짜 원인 미식별**. 11.3h 모니터 동안 fix path 자체가 발화 X — 안정성 회복은 환경 변화 (cam server state cleanup) 때문일 가능성. 다음 자연 stuck 시 fix 실효성 검증.

**Escalation 순서 (stuck 재발 시)**:
1. Step 1 (현재): 자연 stuck 대기 + monitor + fix path 발화 검증
2. Step 2: `GST_DEBUG=2,rtspsrc:5,udpsrc:5,rtpsession:5` env var 추가 후 재실행 — GStreamer 내부 추적
3. Step 3: tcpdump packet capture (RTP/RTCP/RTSP)
4. Step 4 (최후): `37dae37` parent commit (GStreamer 통합 직전, happytimesoft 시점) 빌드 + 동일 환경 A/B test. happytimesoft 도 stuck 시 외부 원인 / 멀쩡하면 GStreamer 통합 결함

자세한 배경: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md)

### CLOSE-WAIT 잔존 — DetectBase 측 socket close 누락 (defensive 추가 후보)
5/26 wd 회귀 조사 시 발견. cam server 가 끊었을 때 DetectBase 가 socket close 누락 → CLOSE-WAIT 패턴. fd leak 누적 위험은 낮으나 defensive 코드 추가 후보 (future, 우선순위 낮음).

### 24일 storm — accept as baseline
- 정밀 mechanism: INF push mutex contention (40x) + inflight_q drop-oldest → correlation_id mismatch
- 24h 중 3회 (~10분), self-healing, fix 비용 > 효익
- 처리 방침: accept. 6+ cam scale-up 시 batch>1 도입 검토 (별도)

### GStreamer GMainContext 공유 (5/26 분석)
- 각 `GstRtspReceiver` instance 가 `g_main_loop_new(nullptr, FALSE)` → default global GMainContext 공유
- multi-cam 환경에서 잠재적 coupling 원인
- Full reset 에는 큰 dip 없음, critical 아님. 미래 cleanup 후보

### NPU batch_size — code 가 batch=1 hard-assumed
[YoloV5_Torch_Onnx_RKNN_NPU.cpp:412](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L412) 잘못된 검증 + [L512](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L512) `input.size = rknn_model_w * rknn_model_h * rknn_model_c` 단일 frame 만 할당. 6+ cam scale-up 시 NPU 천장 도달 시 batch>1 검토 + RKNN 모델 batch>1 재변환 필요.

### v2.0.0 Multi-engine (Search 등) 도입
- [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) 참고
- MAIA event-driven 패턴, NPU 부담 미미 (~2.4 inference/sec)
- Phase 1-5 단계 ~3-4주

### monitor.sh threshold tuning
- 현 기본값: `ALERT_DFPS_LOW_THRESHOLD=100`, `ALERT_DFPS_LOW_STREAK=2`, `ALERT_RSS_MB_THRESHOLD=1100`, `ALERT_WARN_DELTA_PER_CYCLE=500`, `ALERT_WARMUP_CYCLES=4`
- 운영 1-2주 데이터 누적 후 임계값 재검토

### SafeThread → ThreadPool 도입 (scale-up 시점)
- 현 SafeThread 29건 사용, cam 별 인스턴스 분리 (no pool)
- 카메라 8~16대 확장 계획 있을 시 검토. 6+ cam scale-up 의사결정 후

### DeviceCluster 인라인화
- 1개 파일 사용 (SettingManager) → 흡수 가능. 작은 정리.

### v1.0.0 후 GStreamer 1.20.3 → 1.24+ upgrade
- rtpmanager long-running leak (외부 lib, ~340 MB/year, accepted) 의 fix 가능성 검증
- 비용: 1.5~2시간 + Ubuntu 22.04 → 24.04 base 변경 + librknnrt ABI 호환 위험 + protobuf/grpc source rebuild
- 시점: v1.0.0 release 후. 1.24 changelog 에 명확한 본 케이스 fix 단서는 없음 → 효과 불확실
- Bonus: cam_loss root cause [5] (GStreamer thread join 실패) 가 1.24 에서 fix 됐다면 우리 fix 의 leak 압력도 해소

---

## 운영 metric / monitor

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge), 계산: `total_inferences_in_interval / interval`

### canonical monitor
- `logs/monitor.sh <label>` — JSONL, 70+ per-cam fields, 1분 단위 + threshold alerts 7종 (`★storm` / `★err` / `★dfps_low` / `★memory` / `★watchdog` / `★ftc` / `★cam_loss`) + warmup grace 4 cycle
- env override: `ALERT_WARN_DELTA_PER_CYCLE` / `ALERT_DFPS_LOW_THRESHOLD` / `ALERT_DFPS_LOW_STREAK` / `ALERT_RSS_MB_THRESHOLD` / `ALERT_WARMUP_CYCLES`

### single-instance lock
- `/DetectBase/logs/.detectbase.lock` — `flock(2)` advisory lock, Main.cpp 부팅 시 획득
- 두 번째 instance 시도 시 `[FATAL] another DetectBase instance is running` exit 3

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.22) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + master merge gate + git workflow 정책 (source of truth) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 Search engine 도입 가이드 |
| [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md) | cam_loss / GStreamer stuck 분석 (escalation playbook 포함) |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [master_logs/v0.1.18/](../master_logs/v0.1.18/) | v0.1.18 머지 증빙 archival (audit + monitor JSONL + README) |
