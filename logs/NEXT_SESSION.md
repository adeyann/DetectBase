# NEXT_SESSION — DFPS dip 진단 우선 (post-compact 진입점)

**최종 갱신**: 2026-05-26 09:35 KST (context compact 직전)
**현 상태**: `develop` HEAD `7718197` (cmake 0.1.12). Option A architecture + stage FPS counter 도입. **DFPS dip 미해결 — 사용자 가설: Option A 자체 의심**.

---

## 🚨 POST-COMPACT 최상위 task: DFPS dip 진단 (Option A 의심 검증)

### 사용자 가설 (2026-05-26 09:35 KST 기록)
> **"나는 option a를 의심하고 있다."**

Option A (partial reset) 도입 후 DFPS dip 패턴 지속 — pre-Option-A 시점의 운영 안정성과 비교 검증 필요.

### 결정적 관찰 데이터

**post-merge 6.5h+ sanity (monitor `b2so0pcuw`)**:
- 첫 5h: DFPS oscillating 117 ↔ 96, RSS 565~609 stable
- **컨테이너 restart 09:11 KST 후 watchdog 누적 발동 — wd 5 (09:25 시점) → 9 (09:34 시점)**:
  - 09:12 cam 659 16s 무프레임
  - 09:13 cam 658 14s 무프레임
  - 09:17 cam 658 12s 무프레임
  - 09:23 cam 658 14s 무프레임
  - 09:25 cam 659 16s 무프레임
  - **09:34:00 cam 660 12s + 09:34:03 cam 661 13s + 09:34:03 cam 659 12s + 09:34:13 cam 658 15s** ← cluster
- restart 시점에 stage FPS counter 신규 추가 (`859652c`) 됐으니 이 commit 의 부작용도 의심 가능

### 🔥 09:34 cluster 의 결정적 의미 — system-level freeze 가설

09:34 wd cluster 직전 DFPS time series (모든 cam 동시):
```
09:33:34 DFPS=101.7  (4 cam 정상)
09:33:44 DFPS=15.5   ← 모든 cam 동시 추락 시작
09:33:54 DFPS=6.5
09:34:04 DFPS=1.3    ← NotRequest [659 661]
09:34:14 DFPS=20.4   ← NotRequest [658]
09:34:24 DFPS=84.8   ← 회복 시작
09:34:34 DFPS=115.3  ← 완전 정상 복귀
```

**해석**: 14초 사이에 4 cam (IP/RTSP server 모두 다름) 가 동시에 frame 못 받음 → 일제히 watchdog → reset → 복귀. 단일 cam stuck 변종이 아니라 **process-level / system-level 일시 freeze** 패턴.

가설 우선순위:
1. **prometheus mutex contention (stage counter IncrementCounter `859652c`)** — 5 gate 가 hot path 에서 매 frame 동기 mutex 잡음 → 일시 contention storm → cam thread + INF thread 동시 차단 → watchdog
2. **NPU rknn_run 일시 hang** — librknnrt 내부 freeze (5초 timeout 미발동, 14s 의 freeze 는 timeout 안 잡혀 가능)
3. **jemalloc background_thread page reclaim 일시 lock** — bg_thread purge 시 user thread 가 stop
4. **OS-level pause** (CPU steal / swap / GC) — `top`/`pidstat` 으로 확인 필요

### INF-thread profile 분석 (이전 dip 시점, b2so0pcuw 새벽 데이터)
- dip 시점 (DFPS 96): cam thread `total cycle = 34234~34444μs (29 FPS)`, `dq = 21537~23209μs` **정상**
- **camera 수신 정상**, DFPS 는 NPU 추론 통과 frame rate 라 NPU 또는 그 downstream 에서 drop 결론

### Post-compact 우선 작업 순서

1. **현 운영 상태 확인** — `b2so0pcuw` monitor 가 계속 가동 중이면 최신 데이터 확인. 추가 wd cluster 발생했는지 (wd > 9?)
2. **stage counter revert 가설 검증 (가장 빠른 인터벤션)**:
   - `859652c` revert (또는 RegisterStageFpsMetricsOnce 호출만 #if 0) 후 같은 운영 시간 wd 발동 추이 비교
   - 추측: 5 gate IncrementCounter 가 매 frame hot path 에서 prometheus internal mutex 잡음 → 일시 contention storm 으로 4 cam thread + INF thread 가 동시에 차단 → 4 cam 동시 frame_age 12s 초과 cluster
3. **process-level freeze 진단** (revert 와 병행):
   - `pidstat -p <detectbase pid> 1` — freeze 시점에 user/sys CPU 패턴
   - `ps -L -o pid,tid,state,wchan,comm` — freeze 시 thread 들이 어디서 block 중인지
   - `perf record -F 99 -p <pid> -- sleep 60` (freeze 발생 시) — flame graph 로 hot lock 식별
4. **Option A 의심 검증 (stage counter 가 원인 아닐 때)** — pre-Option-A commit (예: `da7412e` Merge fix/rtsp-reconnect-storm) 로 임시 rollback 후 같은 운영 시간동안 cluster 재현 여부
5. **NPU thermal / RKNN runtime 상태** — `/sys/class/thermal/thermal_zone*/temp` + `dmesg | grep rknpu`

### 즉시 시도 가능한 mitigation (우선순위 1)
- **stage counter 5종 IncrementCounter 호출 임시 비활성** (`859652c` revert 또는 5 라인 `#if 0`)
- container restart 후 wd cluster 사라지면 → stage counter 가 원인 확정
- 사라지지 않으면 → Option A 또는 별개 system-level 원인 → 3~4 진단

---

## 🔴 화요일 사용자 자업 (deny list 로 AI 실행 불가)

```bash
# 1. local branch 정리
git branch -D fix/gst-rtpmanager-leak
git branch -D fix/rtsp-reconnect-storm
git branch -D experiment/happytimesoft-rtsp-test
git branch -D experiment/mismatch-root-cause
git branch -D experiment/rtcp-timeout-measure

# 2. (선택) origin merge 완료 branch
git push origin :feature/mpp-integration
```

---

## 🟢 develop 현 상태 (2026-05-26 09:35)

### 최근 commits
```
7718197 docs: NEXT_SESSION + README 갱신
859652c feat(metrics): per-cam stage FPS counter 5종 추가 ⚠️ DFPS dip 의심 commit
7cacbab Merge feature/mpp-integration into develop
5bcd0ae chore: cmake VERSION 0.1.11→0.1.12
861e0b1 revert(rtsp): mppvideodec swap 폐기 — Option A 유지
05f3031 feat(rtsp): Option A partial reset 구조 도입 ⚠️ 사용자 의심 commit
0a4f3c6 / b0e0136 infra(mpp) — 미래 재시도 base
```

### Option A 정보 (의심 대상)
- `code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp` 의 `ResetSourceOnly`
- TeardownSourceSide + BuildSourceSide (source-side 만 swap)
- decode-side (decode_queue + decoder + videoconvert + capsfilter + appsink) 영구 보존
- 측정 reset duration 4~16ms

### Stage FPS counter 정보 (의심 대상)
- `code/Main/DETECTOR/src/RtspDetectorUnit.cpp` 에서 5곳 IncrementCounter 추가
- GATE 4 (line ~1024): `detectbase_cam_avframe_dequeued_total`
- GATE 5 (line ~1283): `detectbase_cam_inflight_pushed_total`
- GATE 6 (line ~1476): `detectbase_cam_result_received_total`
- GATE 7 (line ~1602): `detectbase_cam_tracker_processed_total`
- GATE 8 (line ~1614, 1745): `detectbase_cam_event_dispatched_total`

### 운영 metric 노출 위치 (참고)
- Prometheus endpoint: `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge, InferenceCounter.cpp:143 에서 set)
- DFPS 계산: `total_inferences_in_interval / interval` (즉 NPU 통과 frame rate, 단순 수신율 아님)

---

## 다음 작업 후보 (DFPS dip 진단 후 우선순위)

### NEW-2. `correlation_mismatch` 폭증 root cause
- 현 빈도 0.056/cam/sec (5/24 측정) — surge 사라짐. close 가능.

### A. ThreadProfiler module 신규
- RspProf/InfProf/EvtProf inline struct 통합. ~4시간.

### I. MPP 통합 재시도 (보류)
- library leak upstream fix 대기. infra commits 잔존.

### E. TSan SafeQueue / G. DEBUG virtual lines
- v1.0.0 cleanup 묶음.

---

## 다음 세션 진입 시 자동 처리

1. `docker ps` + 최근 DFPS log 확인 — 운영 컨테이너 상태
2. monitor `b2so0pcuw` persistent 가동 중인지 (이 task 후 종료될 수도, log 확인)
3. 이 NEXT_SESSION 의 "POST-COMPACT 최상위 task" 진입
4. 화요일 잔여 user task 처리됐는지 `git branch` 확인

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
| [logs/post_merge_sanity.csv](post_merge_sanity.csv) | 5h+ 모니터 데이터 (DFPS / RSS / reset / wd / EOS time series) |
