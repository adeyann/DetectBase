# NEXT_SESSION — MPP + Option A 폐기 완료 후 진입점

**최종 갱신**: 2026-05-26 14:00 KST
**현 develop HEAD**: `experiment/mpp-purge` merge 시점 — v0.1.14
**현 상태**: **MPP + Option A 완전 폐기**. Full reset (pre-Option-A) 복귀. 사용자 첫 가설 (Option A 의심) 데이터로 확정 → 폐기.

---

## 🟢 오늘 (5/26) 완료 작업 요약

### 1. stage FPS counter (859652c) revert — v0.1.13
- A-B-A control test 로 wd 빈도 증폭 확인 (container B 5h45m 0 wd vs C 2h30m 11 wd vs D revert 후 0 wd)
- mechanism: `MetricsRegistry::IncrementCounter` 의 global `std::mutex` 가 560 호출/sec hot path 에서 long-tail latency spike
- 미래 재도입 시 Counter& reference 캐싱 필수

### 2. DFPS dip 진단 — 사용자 첫 가설 (Option A) 확정
- Full reset (5/24, .log.2.gz 24h sample 8580): mean DFPS **115.6**, ≥110: **98.8%**
- Option A (5/26 container B, 5h45m sample 345): mean **110.4**, ≥110: **80%**, dip 20%
- mechanism: Option A 14ms partial reset 의 빠른 latency → 4 cam EOS cluster sync 강화 → cluster restart ramp-up 동기화 → DFPS 50~80 으로 ~10초 dip

### 3. MPP + Option A 완전 폐기 — v0.1.14 (이 commit)
- mppvideodec 실제 사용 0 (7a9b32e 시도 후 861e0b1 revert) → Option A 의 본래 목적 사라짐
- 5/24 Full reset baseline 검증된 안정성
- 폐기 대상:
  - `code/Protocol/RTSP_GST/include/GstRtspReceiver.h` — decode/source split 제거, single `pipeline_` 복귀
  - `code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp` — `BuildSourceSide`/`BuildDecodeSide`/`Teardown*Side` 제거, `ResetSourceOnly = TeardownPipeline + BuildPipeline` Full reset 복귀
  - `code/Protocol/RTSP_GST/src/GstRtspClient.cpp` — mppvideodec 관련 주석/메시지 정리
  - `Dockerfile.build` — libmpp + gstreamer-rockchip plugin build steps 제거 (~50 lines)
  - `docker-compose.yml` — `/dev/mpp_service`, `/dev/rga` device mount 제거
- snapshot 보존:
  - git tag `mpp-architecture-snapshot-v0.1.13`
  - `.backup/mpp_purged_20260526/` (Dockerfile / compose / RTSP_GST 전체 + MPP_PURGE_NOTES.md)

---

## 🚀 다음 세션 진입 시 자동 처리

1. `docker ps` + DFPS log 확인 — 운영 컨테이너 상태
2. monitor `b2so0pcuw` (CSV) + `logs/monitor.sh` (JSONL) 가동 중인지 확인
3. v0.1.14 운영 안정성 확인 (5/24 Full reset baseline = mean 115+, ≥110: 98%+ 도달 여부)

---

## 📋 발견한 latent issues (미해결, 후순위)

### GStreamer GMainContext 공유 (5/26 분석)
- 각 `GstRtspReceiver` instance 가 `g_main_loop_new(nullptr, FALSE)` 호출 → **default global GMainContext 공유**
- `gst_bus_add_watch`, `g_timeout_add` 도 default context 사용
- bus message / timer dispatch 가 single thread 직렬화
- multi-cam 환경에서 잠재적 coupling 원인
- 영향: Full reset 시기 (5/24) 에는 큰 dip 없었으므로 critical 아님. 다만 미래 cleanup 후보.

### NEW-2. `correlation_mismatch` 폭증 root cause
- 현 빈도 0.056/cam/sec (5/24 측정) — surge 사라짐. close 가능.

### A. ThreadProfiler module 신규
- RspProf/InfProf/EvtProf inline struct 통합. ~4시간.

### E. TSan SafeQueue / G. DEBUG virtual lines
- v1.0.0 cleanup 묶음.

---

## 운영 metric 노출 위치 (참고)

- Prometheus endpoint: `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge, InferenceCounter.cpp:143 에서 set)
- DFPS 계산: `total_inferences_in_interval / interval` (즉 NPU 통과 frame rate, 단순 수신율 아님)
- monitor.sh (JSONL, 27+ field per-cam) — `logs/monitor.sh <label>` 로 가동

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.14) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + Known Issues |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [.DOCS/STUCK_ANALYSIS_cam659_20260520.md](../.DOCS/STUCK_ANALYSIS_cam659_20260520.md) | cam stuck 변종 B — legacy |
| [.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md](../.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md) | NEW-2 — legacy |
| [logs/audit_20260524_115656/](audit_20260524_115656/) | 최신 audit baseline |
| [logs/post_merge_sanity.csv](post_merge_sanity.csv) | 5/26 monitor 데이터 (B/C/D/E 운영) |

## 복원 (필요시)

```bash
# 전체 MPP + Option A 복원
git checkout mpp-architecture-snapshot-v0.1.13

# 부분 복원
cp .backup/mpp_purged_20260526/Dockerfile.build.snapshot Dockerfile.build
cp .backup/mpp_purged_20260526/docker-compose.yml.snapshot docker-compose.yml
cp -r .backup/mpp_purged_20260526/RTSP_GST_optionA/* code/Protocol/RTSP_GST/
```
