# NEXT_SESSION

**최종 갱신**: 2026-05-27 KST
**현 develop HEAD**: cmake VERSION = `0.1.23`. last master tag = `v0.1.18` (2026-05-27).
**작업 중 branch**: `docs/develop-merge-policy-update` (develop 미머지, HEAD `9bf8c0d`) — git workflow 정책 정정 + DeviceCluster 인라인화 + SafeQueue race review + SafeQueue MO-1 (enqueue+terminate) + CLOSE-WAIT 항목 close 누적.

---

## 📋 남은 작업

### 🟢 검증 대기

#### 1. audit 5종 재검증 (GStreamer GMainContext cleanup 후)
- 0f9ae2c (Receiver/ProxyServer 각자 dedicated GMainContext) 변경 후 cppcheck / clang-tidy / ASan / UBSan / TSan 자체 코드 회귀 검증 필요
- Release 빌드만 통과 확인됨 — Debug 빌드 (audit 시 자동) + 동적 분석 (ASan/TSan) 재실행 필요
- 실행: `./detectbase.sh audit` (전체 5종, ~4-5시간)
- 통과 조건: 자체 코드 결함 baseline (master_logs/v0.1.18/audit_20260527_091456/) 대비 회귀 0건

---

### 🟡 운영 데이터 누적 대기

#### 2. monitor.sh threshold tuning
- 현 기본값: `ALERT_DFPS_LOW_THRESHOLD=100`, `ALERT_DFPS_LOW_STREAK=2`, `ALERT_RSS_MB_THRESHOLD=1100`, `ALERT_WARN_DELTA_PER_CYCLE=500`, `ALERT_WARMUP_CYCLES=4`
- 운영 1-2주 데이터 누적 후 false-positive 분포 확인 → 재검토

#### 3. cam_loss fix path 실효성 검증
- v0.1.18 TeardownPipeline unref-skip 패치 ([GstRtspReceiver.cpp:314-340](../code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp#L314)) 가 사후조치 (defensive workaround)
- 11.3h 모니터 동안 fix path 자체가 발화 X — 다음 자연 stuck 시 검증

**Escalation 순서 (stuck 재발 시)**:
1. Step 1 (현재): 자연 stuck 대기 + monitor + fix path 발화 검증
2. Step 2: `GST_DEBUG=2,rtspsrc:5,udpsrc:5,rtpsession:5` env var 추가 후 재실행
3. Step 3: tcpdump packet capture (RTP/RTCP/RTSP)
4. Step 4 (최후): `37dae37` parent commit (happytimesoft 시점) 빌드 + 동일 환경 A/B test

자세한 배경: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md)

#### 4. 24일 storm — accept as baseline
- 정밀 mechanism: INF push mutex contention (40x) + inflight_q drop-oldest → correlation_id mismatch
- 24h 중 3회 (~10분), self-healing, fix 비용 > 효익
- 처리 방침: **accept**. 6+ cam scale-up 시 batch>1 도입 검토 (별도)

---

### 🟠 scale-up 의사결정 후

#### 5. NPU batch_size 수정 (6+ cam scale-up 시)
- [YoloV5_Torch_Onnx_RKNN_NPU.cpp:412](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L412) 잘못된 batch_size 검증
- [L512](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L512) `input.size = rknn_model_w * rknn_model_h * rknn_model_c` 단일 frame 만 할당
- 6+ cam scale-up 시 NPU 천장 도달 시 batch>1 검토 + RKNN 모델 batch>1 재변환 필요 (3가지 동시 fix)

#### 6. SafeThread → ThreadPool 전환
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

#### 7. GStreamer 1.20.3 → 1.24+ upgrade
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
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.23) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + git workflow 정책 (source of truth) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 Search engine 도입 가이드 |
| [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md) | cam_loss / GStreamer stuck 분석 (escalation playbook 포함) |
| [.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md](../.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md) | SafeQueue race deep review (v1.0.0 진입 전 점검, 자체 race 0건 확정) |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [master_logs/v0.1.18/](../master_logs/v0.1.18/) | v0.1.18 archival (audit + monitor JSONL + README) |
