# NEXT_SESSION — Option A merge 완료 + stage FPS counter 도입

**최종 갱신**: 2026-05-26 09:30 KST
**현 상태**: `develop` HEAD `859652c` (cmake 0.1.12). Option A architecture (partial reset) + stage FPS counter 5종 도입 완료. mppvideodec 는 library leak 으로 보류 (upstream fix 후 재시도).

---

## 🔴 화요일 (2026-05-26) 9시 출근 후 사용자 처리

### 1. local branch 정리 (1줄)
```bash
git branch -D fix/gst-rtpmanager-leak
```
- 이유: origin 은 이미 삭제, local 만 남음. deny list 에 있어 AI 가 실행 불가.

### 2. (선택) 기타 stale local branches 정리
```bash
git branch -D fix/rtsp-reconnect-storm
git branch -D experiment/happytimesoft-rtsp-test
git branch -D experiment/mismatch-root-cause
git branch -D experiment/rtcp-timeout-measure
```

### 3. (선택) origin `feature/mpp-integration` 정리 (merge 완료)
```bash
git push origin :feature/mpp-integration
```

---

## 🟢 develop 현 상태 (2026-05-26)

### 최근 commits
```
859652c feat(metrics): per-cam stage FPS counter 5종 추가
7cacbab Merge feature/mpp-integration into develop
5bcd0ae chore: cmake VERSION 0.1.11→0.1.12
b6fe9a7 docs: NEXT_SESSION 갱신 — MPP 조사 종결
861e0b1 revert(rtsp): mppvideodec swap (7a9b32e) 폐기 — Option A 유지
7a9b32e [revert 됨] feat(rtsp): avdec_h264 → mppvideodec swap
05f3031 feat(rtsp): Option A partial reset 구조 도입 ★ 핵심
0a4f3c6 infra(mpp): libmpp source 를 JeffyCN/mirrors mpp-dev 로 전환
b0e0136 infra(mpp): libmpp 1.0.11 + gstreamer-rockchip plugin Dockerfile
```

### Option A (partial reset) — 검증된 사실
- `GstRtspReceiver::ResetSourceOnly` = TeardownSourceSide + BuildSourceSide
- 매 EOS / 에러 / watchdog 시 source-side 만 swap (decode-side 영구 보존)
- reset duration **4~16ms** (이전 단일 desc 방식 대비 5~10× 빠름)
- avdec_h264 와 함께 5h+ 무회귀 검증 (RSS 565~609 MB stable, leak 0)

### Stage FPS counter (859652c)
8 gateway 의 per-cam frame rate 측정 인프라:
| GATE | Counter | 측정 위치 |
|---|---|---|
| 1 | `detectbase_gst_rtsp_rtp_in_total` | rtspsrc → depay 진입 (RTP) |
| 2 | `detectbase_gst_rtsp_depay_buffer_total` | depay 출력 (per NAL) |
| 3 | `detectbase_gst_rtsp_decoded_total` | appsink (decoded frame) |
| 4 | `detectbase_cam_avframe_dequeued_total` | cam_thread dequeue 성공 |
| 5 | `detectbase_cam_inflight_pushed_total` | NPU 요청 enqueue |
| 6 | `detectbase_cam_result_received_total` | NPU 응답 dequeue |
| 7 | `detectbase_cam_tracker_processed_total` | tracker 통과 |
| 8 | `detectbase_cam_event_dispatched_total` | event 처리 단계 |

Prometheus `rate(<metric>{cam_id="N"}[10s])` 로 stage 별 FPS 계산. DFPS dip 발생 시 어느 stage 가 bottleneck 인지 확정 진단 가능.

### DFPS dip 분석 (5h 새벽 monitoring 결과)
- 패턴: 117 ↔ 96 oscillating, 2h 단위
- INF-thread profile 분석: dip 시점에도 cam thread total cycle = 34ms (29 FPS) **정상**
- **결론**: camera 수신은 정상, NPU 또는 그 downstream 에서 drop
- 다음 dip 시 새 GATE 5/6/7/8 counter 로 정확 단계 식별

### 운영 안정성 (5h+ post-merge monitoring)
- DFPS 평균 ~108 (dip 포함), 최대 117
- resident RSS 565~609 MB stable (무성장)
- reset / EOS 1:1 정확 매칭 (모든 EOS 가 PARTIAL reset 정상 수행)
- watchdog 발동 1회 (restart 직후 cam 659, 메커니즘 정상 작동)

---

## 다음 세션 작업 후보 (priority 순)

### NEW-2. `correlation_mismatch` 폭증 root cause 추적
- 상태: surge 사라짐 (5/24 측정). mismatch 0.056/cam/sec. 사실상 close 가능.
- **DFPS dip 과 연관성** 확인 가능 — stage FPS counter 로 mismatch 발생 시점 stage 별 rate 비교
- 자세히: [logs/MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md)

### DFPS dip 정확 진단 (stage FPS counter 활용)
- 다음 dip 발생 시 GATE 1~8 의 rate 비교 → bottleneck 단계 확정
- 가설:
  - NPU thermal throttling — host `/sys/class/thermal/thermal_zone*/temp` 확인 가능
  - NPU scheduling contention — multi-core 분배 확인
  - inflight_q backlog — GATE 5/6 rate 차이

### A. ThreadProfiler module 신규 작성
- 현재: RspProf/InfProf/EvtProf inline struct 분산
- 목표: 별도 thread 가 모든 stage timing + queue size 일괄 수집
- stage FPS counter 와 통합 자연
- 위치: `code/Profile/ThreadProfiler.h+cpp` 신규
- 비용: ~4시간

### I. MPP 통합 재시도 — **보류 (library fix 대기)**
- rockchip-linux/mpp / JeffyCN/mirrors 의 mpp_destroy 관련 upstream fix 나올 때까지 보류
- infra commits 잔존 (`0a4f3c6`, `b0e0136`) → 재시도 시 base 로 사용
- process auto-restart 운영 wrapper 채택하면 사용 가능 path 도 있음

### E. TSan SafeQueue 추적 한계 (~5건)
- 운영 영향 0, v1.0.0 cleanup 묶음

### G. DEBUG virtual lines 제거 (v1.0.0 시점)

---

## 다음 세션 진입 시 자동 처리

1. `git status` + `git log -7` (현 develop 상태)
2. `git branch -a` (`fix/gst-rtpmanager-leak` 등 stale local 정리됐는지)
3. 이 NEXT_SESSION.md 읽기
4. 운영 컨테이너 상태 확인 (`docker ps`, DFPS log)
5. monitor 가 가동 중이면 (`b2so0pcuw` 같은 persistent) 데이터 확인
6. NEW-2 / DFPS dip 진단 / A (ThreadProfiler) 중 선택

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.12) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + 5 디버깅 원칙 + Known Issues |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [logs/STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) | cam stuck 변종 B 추적 |
| [logs/MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md) | NEW-2 분석 |
| [logs/audit_20260524_115656/](audit_20260524_115656/) | 최신 audit baseline (cppcheck 59 / clang-tidy 0 / TSan 141) |
