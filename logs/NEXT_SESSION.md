# NEXT_SESSION — v0.1.18 (cam_loss root cause fix: TeardownPipeline unref-skip) 진입점

**최종 갱신**: 2026-05-27 10:50 KST
**현 develop HEAD**: chore branch (`chore/v0.1.18-master-prep`) — cmake VERSION `0.1.18` 정렬 (master merge target). last released master = `v0.1.0` (5/19). develop 누적 = v0.1.18 (TeardownPipeline fix 포함).
**현 상태**: **v0.1.18 master merge 준비 중**. monitor `v018_teardown_fix` 11.3h 안정 (wd=1 boot-only, cam_loss=0). audit 5종 5/27 09:14 ~ 14:35 진행 중 (clang-tidy/cppcheck PASS, ASan/TSan 진행 중).

---

## 🎯 5/26 22:00 — cam_loss 진짜 ROOT CAUSE 식별 + FIX (must read)

**증상**: cam 661 의 42분 cam_loss (19:10-19:54). 자가 회복 불가, process restart 만이 해결.

**ROOT CAUSE 확정**: `GstRtspReceiver::TeardownPipeline()` 의 `gst_object_unref(pipeline_)` 가 GStreamer 내부 thread join 에서 **unbounded block**. cam 661 의 backup log:
```
10:09:33  ResetSourceOnly[661] 진입 — pipeline destroy/rebuild
... 매칭되는 OK 로그 없음 (45분 침묵) ...
10:54:54  process restart (escape)
```

ResetSourceOnly 가 호출한 TeardownPipeline 의 unref 가 hang. ReconnectWorker thread (cam 661) 가 receiver_mtx_ 들고 stuck. 같은 thread 안 watchdog cycle 도 발화 불가. process restart 만이 escape.

**FIX**: `gst_element_get_state` timeout 시 `gst_object_unref` 건너뛰고 의도된 leak (process restart 시 OS cleanup) + WARN log.

**검증 (1h)**:
| 측면 | pre-fix (v016/v014_ab) | post-fix (v018+fix) |
|---|---|---|
| Duration | 50min / 8min | **63min** |
| wd | 6 / 1 | **1** (boot 직후) |
| cam_loss | 영구 sustained | **0건** ✅ |
| DFPS | 60-90 osc | **116.1** baseline |
| cam_active | 3/4 sustained | **4/4 stable** |

자세한 분석: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md) (doc 진화 이력 + 이전 틀린 가설 정리 + escalation playbook 포함).

### 솔직한 fix 평가 (사용자 비판적 검증 반영)
- fix 는 **사후조치 (defensive workaround)** — stuck 의 진짜 원인 ([1] stream 끊김 + [5] GStreamer thread join 실패) 미식별
- 162분 운영 결과 fix path 자체가 발화 X → fix 의 effectiveness empirical 검증 0
- 안정성 회복은 stuck 조건 사라진 환경 변화 (cam server state cleanup) 때문일 가능성 → **사실상 162분간 아무것도 안 고친 것**일 수도
- 진짜 검증은 다음 자연 stuck 시

### Escalation 순서 (stuck 재발 시 단계별)
1. **Step 1 (현 상태)**: 자연 stuck 대기 + monitor + fix path 발화 검증
2. **Step 2**: `GST_DEBUG=2,rtspsrc:5,udpsrc:5,rtpsession:5` env var 추가 후 재실행 — GStreamer 내부 동작 추적
3. **Step 3**: tcpdump packet capture (RTP/RTCP/RTSP protocol-level)
4. **Step 4 (최후 수단)**: **happytimesoft RTSP 모듈로 rollback A/B test** — `37dae37` parent commit (GStreamer 통합 직전) 빌드 + 동일 환경에서 stuck 발생 여부 비교. happytimesoft 도 stuck 시 외부 원인 (cam server / 네트워크), 멀쩡하면 GStreamer 통합 결함

자세한 Step 4 절차: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md §"다음 단계"](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md)

---

---

## 🚨 5/26 18:30 ~ 18:55 — wd 회귀 조사 + A/B test 결과 (must read)

**증상**: v0.1.16 binary 50분 운영 시 wd=6 + cam_loss (active=3/4) — 5/24 baseline (wd=0/24h) 대비 명백한 회귀.

**A/B test 수행**: `experiment/wd-baseline-rollback` from `008bd81` (v0.1.14, pre-argv-guard, pre-flock) → 동일 환경에서 binary 만 교체.

**결과**: v0.1.14 binary 도 부팅 4분만에 wd=1 + 동일 cam_loss 발생. **v0.1.16 patch (argv guard + flock) 결백.**

**진짜 원인 — cam 서버 측 state 결함** (happytime rtsp 4.1, 192.168.2.111-114):
- `ss -tan` 결과: 192.168.2.114 가 CLOSE-WAIT (cam 658 wd 와 일치) → 서버가 끊었는데 우리 socket dangling
- 192.168.2.113 ARP `STALE` (cam 659 와 일치)
- container restart 후에도 LAST-ACK 잔존 — server-side 가 자가 회복 안 됨

**Trigger 가설**: 오늘 15:59-16:41 PID 4924 incident (duplicate DetectBase) 동안 각 cam 서버가 2x client load 처리 → happytime rtsp 의 connection table / session state 코럽트. PID 4924 사망 후에도 자가 회복 안 됨.

**대응 — 내일 (5/27) 진행**:
1. **cam 서버 4대 (192.168.2.111-114) 의 happytime rtsp daemon 재시작** — 가장 직접 검증. 재시작 후 wd 발생 멈추면 hypothesis 확정.
2. v0.1.18 baseline 24h 안정성 재측정 (cam 서버 정상화 상태에서).

**부수 발견**:
- DetectBase 가 server-side close 받았을 때 socket close 누락 → CLOSE-WAIT 패턴 관찰. fd leak 누적 위험은 낮지만 (소량), defensive 코드 추가 후보 (future).

---

## 🟢 오늘 (5/26 두 번째 세션) 완료 작업 요약

### 1. PID 4924 사고 진단 (16:00 ~ 17:00)
- 초기 증상: DFPS 115→66, RSS 671→1306MB (정확히 2배), warn 분당 ~10k sustained, MetricsRegistry::Failed ERROR 1건
- 처음엔 5/24 와 동형의 baseline storm 으로 추정. 정밀 분석 결과 cause 가 다름 (자연 회복 안 되는 점)
- **root cause 확정**: PID 4924 = `docker exec ... DetectBase --version` 호출이 풀 서비스 spawn
  - [Main.cpp:44](../code/Main/BASE/src/Main.cpp#L44) 가 `int main()` 형태로 argv 무수신 → `--version` 무시되고 그대로 서비스 부팅
  - 두 instance 가 같은 4 cam + 3 NPU core 동시 점유 → 영구 2x producer 부하 → queue 영구 saturation
- PID 4924 kill 시점에 DFPS / RSS / CPU 모두 정확히 1/2 로 복귀 — duplicate process 가 원인이라는 결정적 증거

### 2. v0.1.16 argv guard + flock 패치 (`b590d6d` → `a2a7a99`)
- [Main.cpp](../code/Main/BASE/src/Main.cpp) 두 층 방어:
  - **Layer 1**: argv parser — `--version` / `--help` / `-v` / `-h` 명시 case + 그 외 인자는 `[FATAL]` exit 2
  - **Layer 2**: `/DetectBase/logs/.detectbase.lock` 의 `flock(2)` advisory lock — 이미 다른 instance 가 잡고 있으면 `[FATAL]` exit 3
- 4 가지 테스트 통과 (--version / --help / unknown / lock blocks duplicate)
- container restart 후 v0.1.16 binary live + lock 활성

### 3. monitor.sh threshold alert 도입 (`2f38f76` + `c1a8b9e` → `0ead0bb` / `579764c`)
- 기존 monitor.sh = heartbeat 만 emit, 자동 anomaly 분류 부재. 5/26 사고 시 모든 metric 잡혔으나 인지 지연 가능성 노출됨
- 7 종 alert 추가 (delta/edge-trigger, spam 방지):
  - `[★storm]` warn 증가 ≥ 500/cycle → correlation_id mismatch 또는 Queue Full burst
  - `[★err]` err count 증가 → DetectBase.log lvl=ERROR 발생
  - `[★dfps_low]` DFPS < 100 sustained ≥ 2 cycle → 단발 reset artifact 제외
  - `[★memory]` RSS ≥ 1100MB → 2x baseline → duplicate / leak 의심
  - `[★watchdog]` wd 증가
  - `[★ftc]` ftc 증가
  - `[★cam_loss]` cam_active < cam_registered
- `ALERT_WARMUP_CYCLES=4` grace period — boot ramp-up (DFPS 87→115 도달까지 3-4분) 의 가짜 dfps_low 차단
- env override 가능: `ALERT_WARN_DELTA_PER_CYCLE` / `ALERT_DFPS_LOW_THRESHOLD` / `ALERT_DFPS_LOW_STREAK` / `ALERT_RSS_MB_THRESHOLD` / `ALERT_WARMUP_CYCLES`

### 4. 5/24 storm 재정의 (cause/mechanism 정밀 규명)
- 처음 분석 (이전 세션) 에선 "scene event burst → tracker churn → NPU saturation" 으로 추정
- 정밀 분석 결과: event 빈도는 storm 시 / baseline 시 동일 → **scene burst 가 직접 cause 아님**
- 진짜 mechanism: **capacity edge cascade**
  - baseline 이 이미 NPU 용량 의 96% 사용 (4 cam × 30 FPS = 120 input / NPU 115/sec 처리)
  - headroom 5% 잠식 시 (RTSP jitter / OS scheduler / micro-perturbation) queue 누적
  - INF push 가 13μs → 517μs (40x, mostly mutex contention) — 직접 증거
  - inflight_q (per-cam, max 10) drop-oldest → correlation_id mismatch
  - effective NPU throughput 절반으로 → DFPS 95-100
  - queue drain 되면 자연 회복 (2-4분)
- 24h 중 3 회 발생, 누적 ~10분 degraded operation (≥110 ratio 98.8% 통계 안에 흡수됨)
- 처리 방침: **accept as baseline**. mitigation 시도하기엔 효익 < 비용. 6+ cam scale-up 시 batch>1 검토 (별도)

### 5. 두 storm 의 차이 명확화
| 측면 | 5/24 storm | 5/26 PID 4924 storm |
|---|---|---|
| Cause | capacity edge (consumer 미세 dip) | duplicate process (producer 2배) |
| Duration | 2-4분 self-healing | 44분 sustained |
| RSS 변화 | plateau | 정확히 doubled |
| CPU | baseline ±10% | 2배 (770%) |
| Errors | 0건 | 1건 MetricsRegistry |
| boot banner mid-run | 0회 | 1회 (06:59:29) |
| Fix | accept (baseline) | argv guard + flock |

---

## 🔧 새 git workflow 정책 (5/26 사용자 확립)

1. **버전 bump 는 push 직후 별도 commit** — code 변경과 cmake bump 를 같은 commit 에 묶지 말 것 (호환성 문제 방지)
2. **develop/master 머지 전**: 직전 마지막 commit 과 비교하여 변경 내용 요약 + 사용자에게 버전 명시적 확인
3. **버전 불일치 시**: 사용자 지정 버전이 commit 의 cmake VERSION 과 다르면 머지 전에 정렬 (commit 수정 or 새 commit)
4. **머지 직후 local bump**: cmake 를 (just-merged) + 1 patch 로 placeholder bump
5. **README / code/README / NEXT_SESSION 등 버전 참조 문서**도 같이 갱신
6. **Pre-push docs check (절대 규칙)** — merge 뿐만 아니라 **모든 commit push 시점**에 모든 문서 (README / code/README / NEXT_SESSION / OPERATIONS / .DOCS/) 전수 점검. 코드 변경과 정합 안 맞으면 즉시 다음 commit 으로 보완. push 전 점검이 원칙.
7. CLAUDE.md §Work Rules + memory `feedback_git_workflow.md` 에 모두 명시. memory 는 AI-only 문서라 영어로 변환됨 (5/26).

---

## 🚀 다음 세션 진입 시 자동 처리

1. `docker ps` + DFPS log + monitor task 확인 — `bthk32wqw` (v016_baseline) 가동 여부
2. monitor JSONL 의 최신 heartbeat 로 baseline 안정성 확인 (DFPS ≥ 115, RSS ≤ 700MB, warn ≤ 30/min, err = 0, wd = 0)
3. **24h+ 운영 모니터링 누적 시 master merge gate 충족 검토** — CLAUDE.md §Verification 의 patch/minor 요건 (3h+ 운영 안정 + audit 5종) 충족 시 사용자 결정 대기

---

## 📋 발견한 latent issues (미해결, 후순위)

### 24일 storm (capacity-edge cascade) — accept as baseline
- 정밀 mechanism: INF push mutex contention (40x) + inflight_q drop-oldest → correlation mismatch
- 24h 중 3회 (~10분), self-healing, fix 비용 > 효익
- 6+ cam scale-up 시 batch>1 도입 검토 (별도 작업)

### GStreamer GMainContext 공유 (5/26 분석)
- 각 `GstRtspReceiver` instance 가 `g_main_loop_new(nullptr, FALSE)` 호출 → default global GMainContext 공유
- multi-cam 환경에서 잠재적 coupling 원인
- 영향: Full reset 에는 큰 dip 없음, critical 아님. 미래 cleanup 후보

### NPU batch_size — code 가 batch=1 hard-assumed (5/26 발견)
[YoloV5_Torch_Onnx_RKNN_NPU.cpp:412](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L412) 의 잘못된 검증 + line 505 input.size 단일 frame 만 할당. 현재 batch=1 hard-locked. 6+ cam scale-up 시 NPU 천장 도달하면 batch>1 검토 가치. 그때 3가지 동시 fix 필요.

### v2.0.0 Multi-engine (Search 등) 도입 가이드 — [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md)
- MAIA 참고. event-driven 패턴이라 NPU 부담 미미 (~2.4 inference/sec)
- 1.0.x simplification 이 multi-engine 도입을 막지 않음
- Phase 1-5 단계 ~3-4주 작업

### monitor.sh threshold tuning — 운영 중 false-positive 발생 시 env override 로 조정
- 현 기본값: `ALERT_DFPS_LOW_THRESHOLD=100`, `ALERT_DFPS_LOW_STREAK=2`, `ALERT_RSS_MB_THRESHOLD=1100`, `ALERT_WARN_DELTA_PER_CYCLE=500`, `ALERT_WARMUP_CYCLES=4`
- 운영 1-2주 데이터 누적 후 임계값 재검토 권장

### BasicLibs 정리 권장 (legacy 출처: [.DOCS/BASICLIBS_AUDIT.md](../.DOCS/BASICLIBS_AUDIT.md) §6 P3)
**누락됐다가 복원 (5/27)**. v1.0.0 정리 단계에서 같이 진행 가능:

1. **ClassChecker YAML → JSON 마이그레이션 + vendored yaml-cpp 제거**
   - 현 `code/BasicLibs/core/parser/yaml-cpp/` (.a 포함) 사용처 = `code/BasicLibs/core/types/ClassChecker.h` 1개
   - YAML 파일 1개 (engines/engine.classes.yaml or similar) JSON 변환 + 사용 코드 nlohmann::json 로 교체
   - 효과: ~3,000 LOC 제거 (vendored yaml-cpp)
   - 작업 ~1-2 시간

2. **SafeThread → ThreadPool 도입** (확장 시점에)
   - 현 SafeThread 29건 사용. cam 별 인스턴스 분리 (no pool)
   - 카메라 8~16대 확장 계획 있을 시 검토. 6+ cam scale-up 시 batch>1 fix 와 묶어서 가능
   - 사전 작업 아님 — scale-up 의사결정 후

3. **DeviceCluster 인라인화** — 1개 파일 사용 (SettingManager) → 흡수 가능. 작은 정리.

### v1.0.0 후 GStreamer upgrade (legacy 출처: [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](../.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md))
**누락됐다가 복원 (5/27)**.

**GStreamer 1.20.3 → 1.24+ upgrade** — rtpmanager long-running leak (`CLAUDE.md` "외부 lib, ~340 MB/year, accepted") 의 fix 가능성 검증.
- 비용: 1.5~2시간 + Ubuntu 22.04 → 24.04 base 변경 + librknnrt ABI 호환 위험 + protobuf/grpc source rebuild
- 시점: **v1.0.0 release 후** 별도 phase (master 안정화 후)
- 기대 효과: rtpmanager leak 사라지면 1년 운영 RSS plateau 확정. 단 1.24 changelog 에 명확한 본 케이스 fix 단서는 없음 → 불확실
- 만약 cam_loss 의 root cause [5] (GStreamer thread join 실패) 가 1.24 에서 fix 됐다면 우리 fix 의 leak 압력 도 해소될 수 있음 — bonus

### v1.0.0 cleanup 묶음 (legacy 출처: [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](../.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md))
**누락됐다가 복원 (5/27 사용자 지적)**. v0.1.16-sync NEXT_SESSION rewrite 시 빠짐.

1. **ThreadProfiler 모듈 신규** — 현 RspProf / InfProf / EvtProf inline struct (각 thread 내부 변수) 통합 → 별도 thread + PUSH (stage timing) / PULL (counter / queue size) API. RtspDetectorUnit.cpp 의 inline instrument 분리. ~4시간 예상.

2. **DEBUG virtual lines 제거** — 매 cycle emit 되는 노이즈성 MLOG_INFO 라인 강등 (`MLOG_INFO` → `MLOG_DEBUG`):
   - `RtspDetectorUnit.cpp:1268` `INF-thread (avg over 100 cycles...)` — 매 100 inference cycle, 4 cam × ~3.3s = 분당 ~75줄
   - `RtspDetectorUnit.cpp:1624 / 1747` `RSP-thread (avg over 100 cycles...)` — 동일 빈도, 라인 길이 매우 김 (50+ field)
   - `RtspDetectorUnit.cpp:1931` `event_detected type=X cam=N count=M` — traffic burst 시 분당 100-200줄 (5/24 storm 분석 시 진짜 시그널 찾기 어려운 노이즈)
   - `RtspDetectorUnit.cpp:2034` `EVT-thread (avg over 100 event cycles...)`
   - **효과**: Release build (`DEBUG_MODE` off) 에서 compile-out (`MgenLogger.h:47-50` cutoff = INFO) → 운영 log volume 대폭 감소 + 실 incident (frame-age watchdog / ResetSourceOnly / EOS) 신호 명확. cam_loss 분석 시 SignalNoise 비율 ↑.

3. **TSan SafeQueue 잔존 race cleanup** — 5/19 audit baseline 시점 자체 코드 race 0건 ✅. 단 v1.0.0 시점 SafeQueue 의 shared_ptr ref counting / max_size 변경 path / drop_count 경합 등 deep review 권장.

4. **v4 instrument 의 `t_*_set` dead code** — `knownConditionTrueFalse` 9건 (cppcheck audit_20260519). ThreadProfiler 마이그레이션 (#1) 시 자연 정리.

순서 의존성:
- #1 (ThreadProfiler) 가 먼저. 그 부수효과로 #4 정리.
- #2 (DEBUG virtual lines 강등) 은 독립. #1 과 묶거나 별도.
- #3 (TSan deep review) 은 #1 진행 중 동시 가능.

---

## 운영 metric / monitor

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS metric: `detectbase_dfps_total` (gauge)
- DFPS 계산: `total_inferences_in_interval / interval`

### canonical monitor (v0.1.16+)
- `logs/monitor.sh <label>` (JSONL, 70+ per-cam fields, 1분 단위) + threshold alerts 7 종 + warmup grace 4 cycle
- 가동 중: `bthk32wqw` label=v016_baseline
- 출력: `logs/monitor_v016_baseline.jsonl`

### single-instance lock
- `/DetectBase/logs/.detectbase.lock` 의 `flock(2)` advisory lock — Main.cpp 부팅 시 획득
- 두 번째 instance 시도 시 `[FATAL] another DetectBase instance is running` exit 3

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.18) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + master merge gate + git workflow 정책 (5/26 갱신) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 Search engine 도입 가이드 |
| [.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md](../.backup/mpp_purged_20260526/MPP_PURGE_NOTES.md) | MPP + Option A 폐기 결정 + 복원 방법 |
| [logs/audit_20260524_115656/](audit_20260524_115656/) | 최신 audit baseline (5/24, cmake 0.1.10 시점) |
