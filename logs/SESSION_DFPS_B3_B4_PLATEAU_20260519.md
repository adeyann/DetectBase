# Session: dfps drift fix (B3 + B4) + 12h plateau 검증 (2026-05-18 ~ 19)

## 시작 상태 (이전 세션 종료 시점)
- branch `feature/dfps-step2-async-inference` @ `ab52bc3`
- B2 async pipeline (InfThread → inflight_q → RspThread) 적용
- inflight_q cap 60 + interval 제거 → dfps 70 → 116.5
- 이슈: inflight_q drift (RspThread cycle 가 cam interval 와 평균 동등) → 5h 후 cap 도달

## 진행 (시간순)

### 1. v3/v4 monitor — drift source 식별
- v3 (10h): inflight=4 cap 으로 누적 폭 80% 감소 검증
- v4: 8 카테고리 instrument 추가 (AVFrame alive counter, ResetSourceOnly counter, SORTTracker kalman count, EngineLoadBalancer pending, /proc/self/maps anon, q_drop 4 큐 등)
- 발견: RspThread cycle 의 99% 가 `resp` (NPU response wait). cycle ≈ cam interval = drain 불가.

### 2. B3 — Event thread 분리
- 신규 `EvtThread` (cam 별 1 개, 총 +4 thread → 16 total)
- 신규 `EventItem` struct (frame shared_ptr + track_results)
- 신규 `event_q` (cam 별 `SafeQueue<EventItem>`, cap=10, drop_oldest)
- RspThread event path (schedule check + BuildNotifyJsonImpl + Convert/clone + io_work + sio + grpc) 전부 EvtThread 로 이전
- RspThread = pop + RespondAsync + tracking + metadata + event_q push 만
- 효과: `ev_us` 2~12ms → **14μs** (99% 감소). 그러나 RspThread cycle 그대로 ~34ms (resp 가 압도적).

### 3. B4 — cam-별 result queue (진짜 fix)
- 원인 식별: `ReplyDispatcher` 가 cam 별 entry **1 개만** (덮어쓰기). RspThread 가 wait_and_get(cam_id) 시 *다음 frame 결과* 도착까지 wait → cycle 가 NPU pacing 에 묶임.
- fix: `EngineLoadBalancer` 안 신규 `cam_result_qs_` (cam 별 `SafeQueue<OutputLayerWrapper>`, cap=10)
- `ReplyDispatcherThreadRunner` 가 `reply_dispatcher_.set_reply` 대신 `cam_result_qs_[uid]->enqueue_move` 호출
- `RespondAsync` 가 `cam_result_qs_[unit_id]->dequeue_wait_for` 호출
- `reply_dispatcher_` 자체는 호환 위해 보존 (deprecated, 사용 X)
- 신규 inspection getter: `GetCamResultQTotalSize()`, `GetCamResultQTotalDrop()`
- 효과:
  - inflight_q 가 backlog 시 RspThread 가 *이미 NPU 처리 끝* 결과 즉시 받음 → drain 가능
  - RspThread cycle (drain path) = 0.2ms (post 작업만)
  - cam pacing 의 묶임 해제

### 4. v8 12h monitor — plateau 검증
- task: `bikjz7pyr` (Monitor 도구, 30 min × 24, 2026-05-18 18:13 ~ 2026-05-19 08:05)
- backup: `logs/MONITOR_V8_bikjz7pyr_20260519_0820.log`

#### 결과
| 항목 | 값 |
|------|------|
| 시작 RSS | 531 MB |
| Plateau RSS range | 602~657 MB (±55 MB oscillating) |
| 종료 RSS | 642 MB |
| HWM 종료 | 705 MB (10h 까지 675, 후반 transient spike) |
| **inflight_q max** | **0~1** (drain 완벽 12h 유지) |
| **avf_alive max** | **4~8** (cap 60 시 100+ 대비 -85%) |
| **q_drop (avf/inf/ev/io)** | **0/0/0/0** (모든 큐, 12h 동안) |
| **resp_wait avg** | 22~25 ms (cap 60 + 덮어쓰기 시 34ms 대비 -27%) |
| DFPS | 116.1~117.1 (정상) |
| ERROR | 0 |
| reset_cnt | 143 (5분 EOS cycle 정상) |

#### B4 효과 비교 (B3 only vs B4)
| Metric | B3 only (cap 10) | B4 (cap 10) | 변화 |
|--------|-----------------|-------------|------|
| inflight_q max | 9 | **0~1** | -86% |
| avf_alive | 38 | **4~8** | -85% |
| RSS plateau | ~721 MB | **~620 MB** | -100 MB (-14%) |
| resp_wait avg | 33.8 ms | **22~25 ms** | -27% |
| drift | progressive | drain 가능 | **영구 해결** |

## 최종 architecture (B4 후)
```
[Camera RTSP] → GstRtspClient → avframe_q (drop>0 정책)
  → InfThread (preprocess + RequestAsync → NPU input_q + inflight_q push)
  → inflight_q (cap=10)
  → RspThread (RespondAsync 이제 cam_result_qs[id] dequeue → tracking → metadata → event_q push)
  → event_q (cap=10)
  → EvtThread (schedule check + BuildNotifyJsonImpl + Convert/clone + io_work + sio + grpc)
  → io_work_queue
  → IOWorkerThread (cv::imwrite)

NPU output flow:
  NPU handler → infer_respond_receiver → ReplyDispatcherThread
  → cam_result_qs[unit_id]->enqueue_move (B4 신규, 이전엔 reply_dispatcher.set_reply 덮어쓰기)
```

## 키 발견 (보존 가치)

### B3 의 결정적 측정
- RspThread total = 34ms 이 cam interval 와 *평균 동등*. push rate = pop rate. drain 불가.
- spike 시 backlog +N. cycle 균형 회복해도 drain 안 됨.
- B3 가 ev_us 14μs 로 줄였지만 *resp_us* 가 압도적 (99%).

### B4 의 결정적 통찰
- `ReplyDispatcher` 가 cam 별 entry **1 개만** (덮어쓰기) — 이게 backlog drain 불가의 진짜 source.
- RspThread 의 *RespondAsync wait* 가 cam 별 *다음 frame 결과* 도착까지 묶임 (NPU pacing).
- 큐로 변경 시 RspThread cycle = post (0.2ms) → drain rate 무한.

### 측정 분해 (v7 instrument)
- resp_us 분해: `resp_wait` (NPU result wait) + `resp_merge` (bbox merge loop)
- v6 측정 resp_us=34ms → wait 가 *bbox merge 가 아니라 NPU pacing* 임을 증명
- bbox merge 7us = 무시 가능

### cap 정책
- v7 시점 cap 10 = drop_oldest 일찍 발동 (이전 cap 60 의 720 MB 누적 폭 → 120 MB 만)
- B4 후 cap 10 = drain 가능해서 거의 안 참 (max 0~1). cap 60 으로 원복 가능 (선택).

## 다음 단계
1. **audit re-run** (cppcheck + clang-tidy + ASan + UBSan) — `detectbase:analysis` rebuild 후 진행 중 (task `blbelbnxu`)
2. **ThreadProfiler 모듈** — instrument 를 별도 thread 로 모음 (제거 X, centralize). RspProf/InfProf/EvtProf inline 측정 → ThreadProfiler 의 PUSH (stage timing) + PULL (counter/queue size) API.
3. inflight_q cap 결정 (10 유지 vs 60 원복)
4. PR `feature/dfps-step2-async-inference` → `develop` → master + `v0.1.0` tag
5. TSan 별도 phase (`./detectbase.sh audit --with-tsan`)
6. cppcheck 79 건 cleanup 별도 PR (모두 style/performance hint)
7. MAIA 호환 RTSP proxy URL 정정 (port 555, mount /<id>) 별도 PR
8. v1.0.0 시점 DEBUG virtual lines 제거

## 사용자 결정사항 (이번 세션)
- Git Flow 변형 채택 (develop branch 영구 + master 보호)
- Squash merge 표준
- 머지 후 미반영 변경 없는 branch delete
- semver 0.x.x (AI 추천 + 사용자 승인)
- Memory 영어 내부 / 한국어 사용자 대면
- Metric 추가 자유 / 제거 사용자 허락
- 외부 release lib leak 없음 전제 (내 코드 안에서 후보)
- inflight_q cap 60 → 10 fix (B4 전 시점)
- cap 10 유지 + monitor 12h 가동 (B4 후)
- audit 먼저 후 PR
- instrument 모으기 (ThreadProfiler 별도 thread, 삭제 X)
