# NEXT_SESSION — v0.1.15 (Full reset 복귀 + REST log fix + monitor 추적) 진입점

**최종 갱신**: 2026-05-26 15:35 KST (context compact 직전)
**현 develop HEAD**: `5590082` (cmake VERSION 0.1.15)
**현 상태**: **MPP + Option A 완전 폐기 후 안정화 단계**. Full reset 복귀. 5/24 baseline 도달 확인 (5/26 15:32 monitor heartbeat: dfps 115.8, reset=28 eos=28 정합, wd=0).

---

## 🟢 오늘 (5/26) 완료 작업 요약 (시간 순)

### 1. stage FPS counter (859652c) revert — v0.1.13 (b451150)
- A-B-A control test: container B (5h45m, no counter) 0 wd vs C (2h30m, counter) 11 wd vs D (revert) 0 wd
- mechanism: `MetricsRegistry::IncrementCounter` 의 global `std::mutex` × 560 호출/sec hot path → long-tail latency spike
- 미래 재도입 시 Counter& reference 캐싱 필수

### 2. DFPS dip 원인 확정 — 사용자 첫 가설 (Option A) 데이터 검증
- Full reset (5/24 .log.2.gz 24h): mean DFPS **115.6**, ≥110: **98.8%**
- Option A (5/26 container B 5h45m): mean **110.4**, ≥110: **80%**, dip 20%
- mechanism: Option A 14ms partial reset 의 빠른 latency → 4 cam EOS cluster sync 강화 → restart ramp-up 동기화 → DFPS 50~80 으로 ~10초 dip

### 3. MPP + Option A 완전 폐기 — v0.1.14 (008bd81)
- mppvideodec 실제 사용 0 → Option A 의 본래 목적 사라짐
- 5/24 Full reset baseline = 검증된 안정성
- 폐기 대상:
  - `code/Protocol/RTSP_GST/include/GstRtspReceiver.h` — decode/source split 제거, single `pipeline_` 복귀
  - `code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp` — Build/TeardownSourceSide·DecodeSide 제거, ResetSourceOnly = TeardownPipeline + BuildPipeline (Full reset)
  - `code/Protocol/RTSP_GST/src/GstRtspClient.cpp` — mppvideodec 주석/메시지 정리
  - `Dockerfile.build` — libmpp + gstreamer-rockchip plugin build steps 제거 (~50 lines)
  - `docker-compose.yml` — `/dev/mpp_service`, `/dev/rga` device mount 제거
- snapshot 보존:
  - git tag `mpp-architecture-snapshot-v0.1.13` (origin push)
  - `.backup/mpp_purged_20260526/` (Dockerfile/compose snapshot + RTSP_GST 전체 + MPP_PURGE_NOTES.md)

### 4. REST silent catch fix — v0.1.15 (675bb67)
- `code/Protocol/REST/src/rest_impl.cpp:55-` 의 catch silent → MLOG_WARN 추가
- 두 overload (Response& / const std::string&) 모두 동일 패턴
- behavior 변화 0 (여전히 빈 `nlohmann::json::object()` 반환)
- 코드 전체 ERROR/WARN 로그 audit 결과 발견된 **유일한 진짜 silent error path**

### 5. monitor.sh fix + git 추적 시작 (3cb4844)
- RESET 패턴: `OK — PARTIAL reset` (Option A) → `ResetSourceOnly\[[0-9]+\] OK` 정규식 (Full reset 도 매치)
- Full reset 운영 시 RESET=0 measurement bug 검출됨 (v014_baseline monitor 31min heartbeat: eos=28, reset=0)
- `.gitignore` 에 `!logs/monitor.sh` exception — canonical script 추적

### 6. master merge gate 규칙 추가 — CLAUDE.md (7ce383d)
- patch/minor bump: audit 5종 + **3h+** 모니터링 + 사용자 명시 허가
- major bump: audit 5종 + **각 10h+** aging + **10h+** stress + 사용자 명시 허가
- memory `feedback_git_workflow.md` 동기 갱신

### 7. OPERATIONS — sanitizer 환경 Queue Full WARN 명시 (c5b3c6b)
- §3 WARN 표에 `Engine Input Queue Full` entry 추가
- §6 audit baseline 에 "Sanitizer 환경 throughput artifact" 섹션 — ASan 2-3x slowdown 으로 NPU bottleneck → backpressure 정상 작동, 빈도 무시

### 8. v2.0.0 multi-engine 설계 문서 — .DOCS/ 추가 (61da783)
- [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md)
- MAIA 분석 — Search 호출 = event-driven (NPU 부담 ~2.4 inference/sec)
- DetectBase 1.0.x 잔존 infrastructure (EngineProfile/Map/Parser/SearchEngineUUID) 확인
- 도입 단계 (Phase 1-5, ~3-4주) + master merge gate 요건

---

## 🚀 다음 세션 진입 시 자동 처리

1. `docker ps` + DFPS log 확인 — container 가동 상태
2. monitor 가동 확인:
   - `monitor.sh v014_baseline` (Monitor tool task `bejn0j5p6`, persistent) — JSONL, per-cam 70+ field
   - 출력: `logs/monitor_v014_baseline.jsonl` + `.out`
3. v0.1.15 운영 안정성 확인 (5/24 Full reset baseline 도달 여부):
   - mean DFPS ≥ 115
   - ≥110 비율 ≥ 98%
   - wd 빈도 ≤ 1/day
   - reset (Full reset) 정상 패턴 — 5분 cycle 마다 cam 별 reset

### 모니터링 데이터 historical archive (5/26 작업)
- `.backup/monitoring_legacy_20260526/post_merge_sanity.csv` (container B/C/D 데이터)
- `.backup/monitoring_legacy_20260526/post_merge_sanity_monitor.sh` (옛 script)

---

## 📋 발견한 latent issues (미해결, 후순위)

### GStreamer GMainContext 공유 (5/26 분석)
- 각 `GstRtspReceiver` instance 가 `g_main_loop_new(nullptr, FALSE)` 호출 → **default global GMainContext 공유**
- `gst_bus_add_watch`, `g_timeout_add` 도 default context 사용
- bus message / timer dispatch 가 single thread 직렬화
- multi-cam 환경에서 잠재적 coupling 원인
- 영향: Full reset (5/24) 에는 큰 dip 없었으므로 critical 아님. 다만 미래 cleanup 후보.

### NPU batch_size — code 가 batch=1 hard-assumed (5/26 발견)

`engines/engine.profile.json` 의 `InferenceBatchSize` 와 RKNN model 의 input tensor batch 차원이 일치해야 한다. 그런데 현 코드의 검증 + buffer 할당이 **batch=1 환경에서만 우연히 동작**:

1. **잘못된 검증** (`code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp:412`)
   ```cpp
   if( batch_size_ != rknn_app_ctx_.io_num.n_input ) { ... return false; }
   ```
   - `io_num.n_input` = **입력 tensor 개수** (YOLOv5 = `"images"` 1개)
   - `batch_size_` = **batch 차원 크기** (profile InferenceBatchSize)
   - 두 개념이 다른데 동등 비교. 우연히 둘 다 1 이라 통과.
   - 진짜 batch dim 검증은 `input_attrs[0].dims[0] vs batch_size_` 비교 해야 함.

2. **input buffer 단일 frame 만 할당** (line 505-517)
   - `input.size = rknn_model_w * rknn_model_h * rknn_model_c;  // batch 무시`
   - batch_size_ 만큼 데이터 들어가야 하는데 buffer 가 1 frame 크기.

3. **accumulator 만 batch 의식** (line 594-609) — buffer 와 일관성 없음

batch>1 사용 시 필요한 fix (3가지 동시):
- line 412 검증 `input_attrs[0].dims[0] != batch_size_` 로 수정
- line 505 input.size `* batch_size_` 반영
- 모델 자체를 batch>1 로 RKNN 재변환

영향: 현재 batch=1 hard-locked. **6+ cam scale-up 시 NPU 천장 도달하면 batch>1 검토할 가치**. 그때 위 3가지 동시 fix 필요.

### v2.0.0 Multi-engine (Search 등) 도입 가이드 작성됨
- 상세: [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md)
- MAIA 참고. event-driven 패턴이라 NPU 부담 미미 (~2.4 inference/sec)
- 1.0.x simplification 이 multi-engine 도입을 막지 않음 — infrastructure (EngineProfile, Map, Parser, SearchEngineUUID) 모두 보존
- Phase 1-5 단계 ~3-4주 작업 추산
- batch>1 fix 와 묶어서 검토 가능

### NEW-2. `correlation_mismatch` 폭증 root cause
- 현 빈도 0.056/cam/sec (5/24 측정) — surge 자연 종료. **close 가능**.

### A. ThreadProfiler module 신규
- RspProf/InfProf/EvtProf inline struct 통합. ~4시간.

### E. TSan SafeQueue / G. DEBUG virtual lines
- v1.0.0 cleanup 묶음.

---

## 운영 metric / monitor

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge, InferenceCounter.cpp:143)
- DFPS 계산: `total_inferences_in_interval / interval` (NPU 통과 frame rate)

### canonical monitor
- `logs/monitor.sh <label>` (JSONL, 27+ field per-cam, 1분 단위)
- 가동 중: `monitor.sh v014_baseline` (Monitor tool task `bejn0j5p6` persistent)
- 출력: `logs/monitor_v014_baseline.jsonl` (append-only)

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.15) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + master merge gate (5/26 추가) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 (sanitizer Queue Full 명시 추가) |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 Search engine 도입 가이드 (MAIA 기반) |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [.DOCS/STUCK_ANALYSIS_cam659_20260520.md](../.DOCS/STUCK_ANALYSIS_cam659_20260520.md) | cam stuck 변종 B — legacy |
| [.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md](../.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md) | NEW-2 — legacy |
| [.DOCS/NPU_MODEL_PERFORMANCE.md](../.DOCS/NPU_MODEL_PERFORMANCE.md) | YOLOv5 s/m/l/x 성능 + cam 수 별 dfps 예측 |
| [logs/audit_20260524_115656/](audit_20260524_115656/) | 최신 audit baseline (5/24, cmake 0.1.10 시점) |

## 복원 (필요시)

```bash
# 전체 MPP + Option A 복원
git checkout mpp-architecture-snapshot-v0.1.13

# 부분 복원
cp .backup/mpp_purged_20260526/Dockerfile.build.snapshot Dockerfile.build
cp .backup/mpp_purged_20260526/docker-compose.yml.snapshot docker-compose.yml
cp -r .backup/mpp_purged_20260526/RTSP_GST_optionA/* code/Protocol/RTSP_GST/
```
