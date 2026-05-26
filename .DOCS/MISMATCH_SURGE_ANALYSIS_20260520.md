# `correlation_mismatch` 폭증 분석 (2026-05-20)

## Summary
PR #16 진단 binary deploy + PR #17 RegisterCounter fix 적용 후 (cmake 0.1.5~0.1.6), `detectbase_correlation_mismatch_total` 가 **이전 baseline 의 ~70× 빈도** (0.4/cam/sec → 27.5/cam/sec) 로 발생. **모든 cam, 모든 mismatch 의 delta = 정확히 10 frame** (variance 0). cross-cam 아닌 cam 내부 NPU response vs frame push 의 stable backlog. **운영 영향**: frame 시차 ~330ms (10 frame × 33ms) → bbox / tracking 위치, event detection 시점 시차. q_drop 0, cam 4/4, system stable.

## Timeline (KST, monitoring `b97mx4ehw` post-PR #17 restart)

| 시각 | label | DFPS | cam | mismatch | log_WARN | enq_drop | errors | prof_RSP ifq/resp | prof_INF push | HWM | Threads |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 10:44:07 | T+0 | 115.4 | 4/4 | 12 | 12 | 0 | 0 | 10363/23485 | 16us (정상) | 534152 | 156 |
| 11:14:17 | T+30 | 115.4 | 4/4 | **489** | 562 | 13 | 0 | 10363/23485 | 16us | 658300 | 156 |
| 11:44:19 | T+60 | 117.1 | 4/4 | **95694** ⚠️ | 96230 | 271 | 137 | **33869/162** ⚠️ | 21us | **1003908** ⚠️ | 156 |
| 12:14:22 | T+90 | 114.8 | 4/4 | **303718** | 304333 | 272 | 137 | 34124/135 | 18us | 1003908 | 156 |
| 12:44:24 | T+120 | 116.7 | 4/4 | **510370** | 511251 | 474 | **326** | 33511/170 | **30us** | 1003908 | 156 |
| 13:14:27 | T+150 | 116.3 | 4/4 | **718738** | 719720 | 484 | 326 | 33535/152 | **41us** | 1003908 | **157** ⚠️ |

**Plateau**: T+60 이후 mismatch 증분 +200K/30min ≈ 110/sec total ≈ 27.5/cam/sec (안정).

## Delta 분포 (3000 mismatch sample, log 직접 parse)

| cam | count | **avg delta** | **max delta** | variance |
|---|---|---|---|---|
| 658 | 749 | **10.00** | **10** | ≈0 |
| 659 | 749 | **10.00** | **11** | ≈0 |
| 660 | 763 | **10.00** | **10** | ≈0 |
| 661 | 739 | **10.00** | **11** | ≈0 |

**결정적 관찰**:
- 매 mismatch 의 delta = **거의 정확히 10 frame**
- cross-cam 이라면 cam 별 delta 분포가 random 또는 시간 의존 — 그러나 모든 cam 일관 = **cam 내부 stable backlog**
- delta = `result_q.correlation_id - inflight.correlation_id = +10` (NPU response 가 inflight 보다 항상 10 frame 앞섬)

## Cross-cam 가능성 검증 → 없음

### Routing 정확 (`EngineLoadBalancer.cpp:316`)
```cpp
const auto uid = opt_respond->meta_data.requester_unit_id;
auto it = this->cam_result_qs_.find( uid );
if( it != cam_result_qs_.end() && it->second ) q = it->second;
if( q ) q->enqueue_move( std::move( *opt_respond ) );
```
NPU response 의 `requester_unit_id` 기반 cam 별 `cam_result_qs_[uid]` 로 push. ResponseThread (`RespondAsync`, line 245) 는 자기 cam 의 result queue 에서만 dequeue.

### `correlation_id` 는 per-cam 독립 (`RtspDetectorUnit.cpp:1191`)
```cpp
uint64_t current_correlation_id = this->frame_count_.fetch_add(1);
```
`frame_count_` 가 RtspDetectorUnit 멤버 atomic. cam 별 0~N sequence. 같은 ID 가 두 cam 에서 동시 존재 가능 — 따라서 cross-cam 비교는 의미 없음.

### 결론
mismatch 는 자기 cam 안의 NPU response queue vs inflight queue 의 ordering 어긋남. delta 일관성이 그 증거.

## Stable Backlog 형성 메커니즘

```
[Service restart 후 초기 transient]
  - NPU 처리 속도 (3-core 병렬 × ~22ms = avg ~7.3ms/frame) > cam thread push 속도 (~33ms/frame, camera ceiling)
  - cam_result_qs 가 누적되기 시작
  - 10 frame 누적 후 cam thread 의 push 속도와 NPU 의 pop 속도가 평균적으로 같아져 stable plateau

[Stable phase 매 cycle]
  - cam thread:     frame N push → inflight_q
                    frame N → RequestAsync → engine_input_q
  - NPU 처리:        frame N 처리 후 → cam_result_qs.push (이미 backlog 10 있어 head 는 frame N-10)
  - ResponseThread: inflight_q.dequeue() = frame N (가장 최근 push 된 것)
                    LoadBalancer.RespondAsync() → cam_result_qs.head = frame N-10
                    mismatch 발생 (delta = +10)
```

## 동시 발생 이상 패턴 (T+60 ~)

T+60 시점에 4가지 metric 변화가 **동시 발생** → 공통 root cause 추정.

### 1. mismatch 폭증 (0.4/cam/sec → 27.5/cam/sec, ~70×)
- T+30 → T+60: 489 → 95694 (+95205)
- 이후 +200K/30min stable

### 2. `prof_RSP` ifq/resp 분포 반전
- 정상: ifq=10000us / resp=24000us (NPU 응답 대기 시간이 resp 안에)
- 비정상: ifq=33500us / resp=150us
- total cycle 동일 (~34ms = camera frame interval) — 단지 wait 위치만 이동
- **해석**: NPU 응답이 inflight push 보다 먼저 도착 (result_q 에 이미 head 있음). RespondAsync 가 즉시 받음 → resp~0. 대신 inflight_q dequeue 가 cam thread push 를 wait → ifq 길어짐.

### 3. HWM 1GB peak (660MB → 1004MB)
- T+60 시점에 도달 후 monotonic 아닌 plateau (T+60 부터 stable 1003908 KB)
- RSS 는 정상 ~630MB 그대로
- 일시적 peak 만 큼, working set 영향 0

### 4. `prof_INF push` 시간 증가 (16us → 41us, monotonic)
- T+30: 16us (정상)
- T+150: 41us (2.5×)
- inflight_q.enqueue_move 의 시간 증가 = lock contention 증가

## Root cause 가설 (PR #16 진단 binary trigger)

PR #16 (`debug/gst-rtsp-stale-trace`) 의 변경 중 hot path 영향 의심:

### 1. `GstRtspReceiver::OnNewSample` 매 frame 마다 atomic store
```cpp
const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch() ).count();
self->last_frame_ns_.store( now_ns, std::memory_order_relaxed );
```
- frame_cb (GStreamer bus thread) 안에서 매 frame 마다 atomic store
- 다른 thread (InferenceThread 의 timeout case 에서 SetGauge 시 GetLastFrameNs() lock 잡고 read) 와 cache line contention 가능성

### 2. `GstRtspReceiver::OnBusMessage` 매 bus message 마다 IncrementCounter
- bus 5분 cycle EOS + STATE_CHANGED 등 다수 type
- prometheus-cpp 의 IncrementCounter 가 mutex 또는 atomic 사용 — 매 호출 cost

### 3. `OnBusMessage` 의 모든 type log
- bus thread 안에서 MLOG_DEBUG/INFO/WARN — 동기 log + JSON serialization
- 미세 지연 누적

이 변경들이 cam thread (InferenceThread) 의 cycle 시간을 미세하게 늘리는 효과 → push 시간 16us → 41us 증가가 그 증거. 그 결과 cam push 속도 < NPU 처리 속도 → result_q backlog 형성.

### 검증 가능한 실험 (별도 PR)
1. PR #16 의 변경 중 일부만 활성화한 빌드 (예: bus message metric 빼기)
2. mismatch 변화 측정 → 어떤 진단 항목이 trigger 인지 식별

## Threads 156 → 157 패턴 (T+150)
- cycle 6 (T+150) 에 +1 발생
- 이전 cam 659 stuck 사건 (2026-05-20 05:42) 의 cycle 8 (T+242) 에서도 동일 +1 발생
- 무슨 thread 인지 미상 — 영구 thread 추정
- cam stuck 과 mismatch surge 두 패턴 모두에서 발생 — 공통 원인일 수 있음 (별도 분석)

## 운영 영향 평가

| 항목 | 정상 | 폭증 시 | 평가 |
|---|---|---|---|
| cam active | 4/4 | 4/4 | ✅ |
| DFPS | 116.5 | 115.4~117.1 | ✅ |
| q_drop (all) | 0 | 0 | ✅ |
| errors metric | 0 | 326 (engine_input_q_drop, 0.04% 비율) | ✅ |
| frame 시차 | 0 | **~330ms** (10 × 33ms) | ⚠️ |
| bbox 위치 정확도 | exact | frame 10 시차 (빠른 객체 시 visible) | ⚠️ |
| tracking ID assignment | exact | 시차 만큼 잘못 매칭 가능성 | ⚠️ |
| event detection (LineIntrusion 등) | exact | 객체 line 통과 시점에 10 frame 시차 | ⚠️ |
| log volume | normal | 50× (mismatch warning) — logrotate 부담 | ⚠️ |
| HWM peak | 660MB | 1004MB stable plateau | ⚠️ (working set 영향 0) |

**결론**: 시스템 정상 운영. 단 frame ordering 정확도 ~330ms 시차 영향. 빠른 객체 또는 timing-critical event 에 영향 가능.

## 다음 단계

### Phase 1 — 진단 (root cause 식별)
1. PR #16 의 진단 항목 중 어느 것이 trigger 인지 식별 (별도 실험 branch)
   - bus message metric IncrementCounter 만 비활성
   - last_frame_ns_ atomic store 만 비활성
   - 둘 다 비활성 (PR #16 효과 0)
2. 각 시나리오 측정 → mismatch 빈도 비교

### Phase 2 — Fix (PR #9 §C 의 옵션)
- **per-correlation_id lookup**: `cam_result_qs_` 를 `vector` → `map<correlation_id, OutputLayerWrapper>` 로 변경. ResponseThread 가 자기 inflight 의 correlation_id 로 직접 lookup → mismatch 0 보장.
- **handler affinity**: cam → 고정 NPU handler 매핑 (round-robin 포기). 그러나 NPU saturation 시 fairness 문제.

### Phase 3 — 검증
- audit 5종 모두 통과
- 10h+ sanity (mismatch 0 baseline)
- 진단 binary 부담 제거 또는 light version 으로 교체

## 관련 PR/문서

- PR #9 `refactor/audit-cleanup` — frame ordering defense counter 추가 (`RtspDetectorUnit.cpp:1441~1450`), 발생 빈도 측정 의도
- PR #16 `debug/gst-rtsp-stale-trace` — 진단 binary (이 surge 의 trigger 추정)
- PR #17 `fix/correlation-mismatch-metric` — `RegisterCounter` 누락 fix (PR #9 결함, metric 노출 가능해짐)
- [logs/STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) — cam 659 stuck (별도 issue, mismatch surge 전 발생)
- [logs/NEXT_SESSION.md](NEXT_SESSION.md) — 다음 작업 우선순위
