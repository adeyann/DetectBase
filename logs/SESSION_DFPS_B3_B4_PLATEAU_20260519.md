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

## audit phase (2026-05-19 08:24 ~ 진행 중)

### audit 1st (08:24) — 실패
- `./detectbase.sh audit` 실행
- 결과: `detectbase:analysis` image 가 5/8 Phase 2 도입 이전 base 사용 (cached). GStreamer dev package 없음 → ASan cmake configure 실패.
- log silent fail (`asan_build.log` 0 줄, `>/dev/null 2>&1` 가 docker exec 안 redirect)

### audit 2nd (08:47, 23분 소요) — image rebuild 후 성공
- `docker build -f Dockerfile.analysis -t detectbase:analysis . --no-cache` (현 detectbase:1.0 base)
- 결과:
  - cppcheck: **79 건** (style/perf only, critical 0)
  - clang-tidy: **166 warning** (1st run 의 0 은 silent fail)
  - ASan/UBSan: **1.2 MB leaked / 10412 allocations** (90s smoke)
  - TSan: 미실행 (`--with-tsan` 옵션)

#### ASan 분석 (90s smoke 한계)
- 5 direct leak: 1×glib startup, 2×NPU AllocateBuffers (YoloV5_Torch_Onnx_RKNN_NPU.cpp:445/458), 1×librknnrt rknn_init, 1×gst_version_string
- 174 indirect leak: 위 direct 의 child allocations
- **진짜 누수 0** — 모두 startup-time allocation 가 graceful shutdown 안 한 90s smoke 라 leak 으로 보임. process 종료 시 OS 가 자동 회수.
- v8 12h monitor (RSS plateau + q_drop 0) 와 일치.

### Main.cpp + detectbase.sh 수정 (long-run ASan 지원)
- `Main.cpp`: `#ifdef __SANITIZE_ADDRESS__` guarded SIGUSR1 handler → `__lsan_do_recoverable_leak_check()` 호출. production 영향 0.
- `detectbase.sh`: `ASAN_DURATION_MIN` 환경변수 (default 240min). interval SIGUSR1 sender (T+5min/20min/50min/110min/230min) background.

### audit 3rd (09:34 ~ 10:56, 1h 23분) — long-run ASan 완료
- `ASAN_DURATION_MIN=60 ./detectbase.sh audit` (task `bf98bchh1`)
- 결과:
  - cppcheck: 79 (동일)
  - clang-tidy: 166 (동일)
  - **ASan: 1217069 byte / 10650 allocations** (2nd 의 1209453 byte / 10412 대비 +7616 byte / +238 alloc, 1h runtime 누적)
  - SIGUSR1 도착: T+5min (300s), T+15min (900s), T+30min (1800s)
- 결과 위치: `logs/audit_20260519_093431/`

#### Runtime leak 확정 (★ 새 발견)

audit 3rd 에서 **새 Direct leak 2건** (1h 동안 누적):

```
Direct leak of 3808 byte(s) in 119 object(s) × 2 (다른 path)
  ↑ glib g_malloc → gstrtpmanager.so → g_object_unref →
    gstreamer set_state → TeardownPipeline (GstRtspReceiver.cpp:185)
    → ResetSourceOnly (GstRtspReceiver.cpp:255)
    → ReconnectWorker (GstRtspClient.cpp:202)
```

**source**: GStreamer **rtpmanager** (rtspsrc 내부) cleanup 의 g_object_unref → g_list_foreach 중 leak.

**메커니즘**: 매 EOS reconnect (5분 cycle, ~12회/h) → `TeardownPipeline()` → `gst_element_set_state(NULL)` 의 cleanup 과정에서 *rtpmanager 의 SSRC list* 가 일부 unref 안 됨.

**크기**: ~32 byte × ~10 alloc / reconn = ~320 byte / reconn
- 12h × 12 reconn/h × 320 byte = ~46 KB / 12h (v8 plateau noise 안)
- 24/7 × 30일 = ~28 MB / month (long-term)
- 24/7 × 1년 = ~340 MB / year

**평가**:
- v3 세션 (5/18) 에서 의심했던 "ResetSourceOnly 마다 ~5 MB 누적" 가설 = **틀렸음** (실제는 ~320 byte / reconn).
- v8 12h monitor 의 RSS plateau (602~657 MB range, ±55 MB oscillating) 안 noise 수준 — 진짜 leak 가 있어도 plateau 깨지지 않음.
- **External lib (GStreamer rtpmanager)** 측 — feedback_leak_in_my_code.md 의 "release lib leak 없음" 전제 위반 사례. 그러나 *known GStreamer issue* 가능성 검토 필요.

#### 다음 단계 — runtime leak fix 옵션

| 옵션 | 효과 | 비용 |
|------|------|------|
| A. 수용 (long-term ~340 MB/year acceptable) | 0 | 0 |
| B. ResetSourceOnly 의 in-place reset (TeardownPipeline 안 호출) — 이미 시도되어 있음 (`mppvideodec 보존` 주석) | 만약 in-place 가능하면 TeardownPipeline 안 거치고 leak 회피 | code 검토 |
| C. GStreamer 업데이트 (rtpmanager bug 가 fix 된 버전) | external lib 의존 | dependency 영향 |
| D. EOS reconnect 빈도 줄이기 (camera mp4 5분 cycle 외 대처) | 12 reconn/h → 적은 빈도 | 운영 정책 변경 |

### clang-tidy 166 warning 분류

| 카테고리 | 수 | 평가 |
|----------|----|------|
| bugprone-narrowing-conversions | 69 | 안전 (대부분 false positive, fix=static_cast 명시) |
| performance-unnecessary-value-param | 44 | 안전 (const ref) |
| bugprone-easily-swappable-parameters | 16 | review (보통 false positive) |
| **bugprone-implicit-widening-of-multiplication-result** | **10** | **잠재 (overflow, 현 input 크기 OK)** |
| performance-move-const-arg | 5 | 안전 |
| performance-no-automatic-move | 4 | 안전 |
| bugprone-branch-clone | 4 | review |
| cppcoreguidelines-pro-type-member-init | 3 | 안전 (POD init) |
| performance-inefficient-vector-operation | 2 | 안전 (reserve) |
| **bugprone-exception-escape** | **2** | **실제 위험 (std::terminate)** |
| performance-unnecessary-copy-initialization | 1 | 안전 |
| **bugprone-use-after-move** | **1** | **잠재 UB** |
| bugprone-string-literal-with-embedded-nul | 1 | review |
| **bugprone-misplaced-widening-cast** | **1** | **잠재 (overflow)** |

→ **잠재 14건** (use-after-move 1 + exception-escape 2 + widening 10 + misplaced-widening 1)
→ **안전 152건** (별도 cleanup PR)

### 잠재 14건 fix (uncommitted, 적용됨)

| # | 위치 | fix | 평가 |
|---|------|-----|------|
| 1 | YoloV5_Torch_Onnx_RKNN_NPU.cpp:517 | `input.size` → `target_input.size` (move 후 사용 X) | use-after-move |
| 2 | SettingData.cpp:344 | `CameraSettingData()` 안 try/catch (사용자 결정: B 옵션) | exception-escape |
| 3 | SettingData.cpp:350 | `CameraSettingData(json)` 안 try/catch | exception-escape |
| 4 | YoloV5_Torch_Onnx_RKNN_NPU.cpp:200,204,209,210,214,222 | `static_cast<size_t>(...)` + `grid_len_sz` | implicit-widening |
| 5 | YoloV5_Torch_Onnx_RKNN_NPU.cpp:571 | `static_cast<size_t>(i) * 3` | implicit-widening |
| 6 | MgenHungarian.cpp:175,186 | `static_cast<size_t>( nOfRows ) * col` | implicit-widening |
| 7 | MgenHungarian.cpp:235 | `static_cast<size_t>( nOfRows ) * col` | implicit-widening |
| 8 | RtspDetectorUnit.cpp:875 | `2 * static_cast<size_t>( detect_fps_limit_ )` (cast 순서) | misplaced-widening |

### 안전 cleanup 일부 적용 (uncommitted, 3건)
- AbnormalActionChecker.cpp:65: `const auto back_up_object` → `auto back_up_object` (std::move 효과)
- InferObject.cpp:39: `bbox(std::move(other.bbox))` → `bbox(other.bbox)` (trivially-copyable)
- InferObject.cpp:65: 동일 (operator=)

**나머지 안전 cleanup ~70 건은 별도 PR (사용자 결정)**.

## 다음 단계 (audit 끝난 후)
1. audit 3rd 결과 분석 — ASan interval leak diff (T+5min/20min/50min) → true runtime leak 식별
2. build 검증 (잠재 14 fix + 3 cleanup) inside detectbase_service container
3. commit + push (현 branch 위)
4. **ThreadProfiler 모듈** — instrument 를 별도 thread 로 모음 (제거 X, centralize). RspProf/InfProf/EvtProf inline 측정 → ThreadProfiler 의 PUSH (stage timing) + PULL (counter/queue size) API.
5. inflight_q cap 결정 (10 유지 vs 60 원복)
6. PR `feature/dfps-step2-async-inference` → `develop` → master + `v0.1.0` tag
7. **별도 cleanup PR (refactor/audit-cleanup)** — 안전 70+ fix:
   - performance-unnecessary-value-param 44
   - bugprone-narrowing-conversions 69 (대부분 false positive)
   - cppcheck style/perf (52건 중 5건 이미 위 잠재 fix 와 같이 적용, 나머지 47건)
   - cppcoreguidelines-pro-type-member-init 3 (struct tm init)
   - performance-* 나머지
8. TSan 별도 phase (`./detectbase.sh audit --with-tsan`)
9. MAIA 호환 RTSP proxy URL 정정 (port 555, mount /<id>) 별도 PR
10. v1.0.0 시점 DEBUG virtual lines 제거

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
