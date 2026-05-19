# Session: dfps 최적화 + Leak Hunt (2026-05-17 ~ 18)

## 시작 상태 (PR #6 머지 직후)
- master `49d3043` (A-plan NPU multi-handler 머지 완료)
- dfps 70 (frame-skip interval 정책으로 over-cap)
- RSS plateau ~575 MB (Phase 2 plateau 검증된 마지막 안정 baseline)

## 작업 흐름 (시간순)

### 1. Profile 결과 (B1 step 0)
InferenceThread main loop 의 stage 측정으로 cycle 71ms 분해:
- dq=26ms (frame 도착 대기)
- pre=11ms (sws_scale, FFmpeg SWS_AREA, 99% hot spot)
- inf=22ms (NPU 왕복)
- post=12ms (tracking+metadata+event+emit)

### 2. B2 async pipeline 구현 (큰 변경)
- 신규 `InflightItem` struct (frame + engines + correlation_id 등 self-contained 복사)
- 신규 `inflight_q_` (`SafeQueue<InflightItem>`, cam 별 1개)
- 신규 `response_thread_` (RspThread, cam 별 1개)
- `InfThread` (cam-thread) 책임 축소: dq → preprocess → RequestAsync → inflight_q push (cycle 34ms)
- `RspThread` (신규): inflight_q pop → RespondAsync → tracking → metadata → event/emit (cycle 34ms)
- `class_id_person_`, `class_id_car_` 멤버화 (read-only RspThread access)
- shutdown 순서: inference → inflight_q.terminate → response → io_worker
- 코드: `code/Main/DETECTOR/{include/RtspDetectorUnit.h, src/RtspDetectorUnit.cpp}`

### 3. Frame-skip interval 정책 제거
- `GstRtspClient::frame_cb` 의 interval(1000/fps_limit ms) drop block 7줄 제거
- 큐 size>0 drop 정책만 유지 (메모리 안전)
- 코드: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp`
- 효과: dfps 70 → 116.5 (+66%)

### 4. dfps 천장 진짜 값 발견 (중요)
- camera_interval 실측 instrument 추가 (`GstRtspClient::GetFrameIntervalAvgUs`)
- 측정값: **camera frame interval 34.3 ms = 29.13 FPS** (30 FPS 가정 잘못)
- 진짜 dfps 천장 = **4 × 29.13 = 116.5**. 현재 측정 116.5 = **100% 도달**.
- "2.9% drop" 라 했던 것 = drop 아니라 camera 자체 frame rate 차이.

### 5. Leak 발견 + 원인 식별
- 10h monitor 시도 시 5h 사이 RSS 580 → 1370 MB (+790 MB) 누적
- 가설 검증 instrument 단계별 추가:
  - 큐 size (avframe_q, inflight_q, io_q, lb_resp, lb_input) → **inflight_q 만 누적**
  - GStreamer 내부 (gst_appsink_buf) → 0
  - jemalloc stats (alloc/active/resident/mapped) → fragmentation 아님 (alloc 도 같이 증가)
  - SORT tracker, scheduler 등 → 안정
- 진짜 원인: **inflight_q 자연 누적**
  - RspThread cycle 34.5ms (event 시 +5~10ms) > InfThread cycle 34.3ms
  - 매 cycle 미세 frame 잔존 → 5h 후 60 cap 도달
  - 60 frame × 3 MB × 4 cam = 720 MB ≈ 측정 RSS 누적과 일치

### 6. Fix: inflight_q max_size 60 → 4
- `RtspDetectorUnit::Init()` 의 `inflight_q_->SetMaxSize(4)` 변경
- 코드: `code/Main/DETECTOR/src/RtspDetectorUnit.cpp:398`
- drop_oldest 일찍 발동 → 누적 폭 720→48 MB
- dfps 영향 없음 (drop_oldest 가 oldest frame 만 버리고 NPU 가 latest 처리)
- 측정: inflight max 0~3 안정, dfps 116.3 유지

## 현재 상태 (2026-05-18 09:10 KST)

### Branch / Commit
- branch: `feature/dfps-step2-async-inference`
- 마지막 commit: `ab52bc3` (WIP: B2 + interval 제거 + inflight=4 + full instrument)
- master 와 차이 큼. PR 머지 안 함.

### 10h monitor v3 가동 중
- task_id: `b7ekq0z7p`
- 측정 간격: 30분 × 20 (10h)
- 시작: ~08:30 KST, 종료 예정: ~18:30 KST
- 목적: inflight=4 후 RSS plateau 도달 vs 잠재 추가 leak 구분

### 최근 측정 (+60min)
- RSS 625 MB (d+14, 이전 inflight=60 의 +70 보다 80% 감소)
- inflight max 2 (cap 4 안)
- DFPS 116.5, ERROR 0
- jem_alloc 366→353 (감소!)
- 모든 큐 안정

## 다음 단계 (10h v3 완료 후)

### (a) plateau 도달 시 — leak 완전 해결
1. instrument 코드 제거 (운영 로그 spam 방지)
2. branch push + PR 생성 → master 머지
3. master 머지 후:
   - dfps 116.5 = 천장 (camera 29.13 FPS × 4 cam) — fix 종료
   - 또는 B3 (tracker/event 별도 thread 분리) 로 RspThread cycle 단축 → cap 60 복귀 가능

### (b) 지속 증가 시 — 추가 bisect
- jemalloc page release 지연 vs 진짜 추가 leak 구분
- 후보: SORT tracker 내부 state, scheduler event history, socket.io 내부 queue (외부 lib)

## 핵심 결정적 발견 (보존 가치)

### Camera frame rate
- IP CCTV camera 가 **30 FPS 가 아니라 29.13 FPS** 실측
- dfps 천장 = camera_FPS × cam_count = 29.13 × 4 = **116.5** (절대)
- 향후 cam 추가 또는 모델 경량화 외에는 더 못 올림

### NPU 천장
- single core inference 17 ms (rknn_run) + 2 ms (outputs_get) = 19 ms per inference
- 1 core throughput = 1000/19 = **52 FPS/core**
- 3 core multi-handler 천장 = 3 × 52 = **156 FPS** (이론, scheduling overhead 빼면 ~142)
- 8 cam 환경 예상 dfps = ~142 (NPU 천장)

### Decoder 천장
- avdec_h264 software CPU 8 core = ~240 FPS 천장
- 8 cam 까지 NPU 가 진짜 bottleneck, 16+ cam 부터 decoder bottleneck

### B2 의 진짜 효과 측정
- cam thread cycle 71 ms → 34 ms (-52%)
- dfps 70 → 116.5 (+66%)
- 단 inflight_q 라는 새 buffer 도입 → cap 정책 신경

## 알려진 이슈 / Deferred

### dfps 추가 향상 (인프라 마련 후 대기)
- B1 sws_scale 최적화 (SWS_BILINEAR / SWS_FAST_BILINEAR / RGA hardware) — 사용자 KIP, 내일 결정
- B3 tracker/event 별도 thread — RspThread cycle 단축 시 inflight=60 으로 복귀 가능 (deferred)

### Phase 2 검증
- VLC GUI 실 viewer 검증 (4 cam 영상 표시 확인) — 내일
- SDP sprop-parameter-sets 미설정 — ffprobe 같은 strict client 첫 1초 디코드 불가

## Memory 추가
- `feedback_perfect_metric.md` (2026-05-17 추가) — 정량 metric 의 "거의 도달" hedging 거부

## 사용자 작업 패턴 (이번 세션 학습)
- "다 측정하라" 명시 시 — 누락 없이 한 번에 추가, 외부 라이브러리 한계 분리 명시
- 알림 짜증 (지속 sleep 대기) 시 — 직접 측정 + 빠른 진행
- "완벽한 것" 요구 — 97% 도달 도 거부, 100% 또는 차이 원인 명시 + fix 후보 제시

## 다음 세션 진입 시 우선순위
1. **10h v3 monitor (b7ekq0z7p) 결과 확인** — plateau 도달 여부 (4 cam baseline)
2. **RspProf stage timing 분석** — RspThread 어느 단계가 cam thread 보다 느림 만드는지 식별
3. 보틀넥 단계 fix (유력: event/emit 분리 = B3)
4. inflight_q cap 60 으로 원복 + 재측정 → 누적 0 확인
5. instrument 정리 + PR
6. 별도: 내일 사용자 GUI 검증 + B1 결정

## 사용자 환경 변경 (2026-05-18 09:50 KST → 09:55 보류)
- 09:50: 메인서버 cam 할당 4 → 6 으로 증가 시도. 다음 service 실행부터 적용 예정.
- **09:55: 4 cam 으로 재할당**. Phase A (4 cam leak hunt + RspThread fix + master 머지) 완주 후 6 cam Phase B 별도 진행.
- (참고용) 6 cam 시 영향:
- 영향:
  - dfps 이론 천장: 29.13 × 6 = 174.8 (이전 116.5)
  - NPU 실측 천장: ~142 (3 handler × 47 FPS, scheduling overhead 포함)
  - 새 진짜 천장 = NPU saturation (camera 천장 아님). 예상 dfps ~142, drop ~18.7%
  - inflight_q 동작 변화: NPU saturation 으로 *모든 cam* inflight_q 가 빠르게 참 → drop_oldest 강제 발동 (자연 누적 아니라 backpressure)
  - thread 수: 8 → 12 (cam 별 InfThread+RspThread × 6)
  - inflight RSS: 48 MB → 72 MB
- 4 cam 분석 결과 (RspThread event burst) 의 적용 가능성 — 6 cam 에서는 NPU 가 먼저 fail 하므로 *덜* 보일 수 있음. 6 cam 환경 재측정 후 우선순위 재정렬.
