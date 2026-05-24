# Frame-age 기반 force-reset 설계 (2026-05-20)

**상태**: 설계 완료, 빌드/배포 미실시 (사용자 지시)
**임계값**: **5초** (사용자 결정)
**대상 branch**: `fix/frame-age-force-reset` (예정, develop fork)

## 배경

2026-05-20 monitoring 중 cam stuck cascading failure 발생:

| cam | 마지막 EOS | Stuck 시작 | 7시간 monitoring 중 stuck 누적 시간 |
|---|---|---|---|
| 660 | 17:05:32 | 17:10:33 | 34.5분 |
| 661 | 17:15:03 | 17:16:30 | 28.6분 |
| 659 | 17:35:22 | 17:37:28 | 7.6분 |
| 658 | 17:43:23 | (alive) | 0 |

T+420min (17:45) 시점: **cam=1/4, DFPS=29.1 (정상 116)**.

### 패턴 (이전 cam 659 stuck 사건과 동일)
1. 5분 cycle EOS reset 정상 진행 (수십 회)
2. 어느 시점 EOS reset 후 stream 정상 재개
3. **1~5분 후 mid-stream 에서 frame 멈춤**
4. 다음 예정 EOS 도 안 옴 → 우리 측 reset trigger 없음
5. TCP ESTAB 유지 (close 안 됨)
6. 무한 stuck

### Root cause 가설 (분기 미확정)
- **A. 외부 RTSP server frozen** — TCP keepalive 만 응답, RTP/RTSP 안 보냄
- **B. 우리 측 rtspsrc internal stale state** — multi-stream 처리 시 progressive race
- TCP packet 직접 capture (tcpdump) 로 가설 분기 가능, 미수행

→ 어느 가설이든 **우리 측 force-reset 으로 mitigation 가능**.

## 설계

### 검출 metric: `last_frame_age_sec`

[GstRtspReceiver.cpp:309-315 OnNewSample()](../code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp#L309-L315):
- pipeline 끝 (appsink) 에서 매 decoded frame 마다 `last_frame_ns_.store(now)`
- **Frame 기준** (packet 아님) — packet 와도 decode stuck 시 stuck 검출, packet 안 와도 stuck 검출

### 임계값: 5초

| 기준 시간 | 의미 |
|---|---|
| 정상 frame 간격 | 33ms (30fps) |
| 정상 EOS reset frame gap | **1.6초** (실측: ResetSourceOnly 20ms + state 전환 150ms + RTSP handshake + jitterbuffer 200ms + 첫 frame decode) |
| 임계값 | **5초** = 정상 reset 의 3× margin |
| 지금 발생한 stuck | 565s ~ 2070s (임계값의 100~400×) |

5초 trade-off:
- 빠른 회복 (stuck 발생 후 ~5초 안에 force reset)
- 정상 EOS reset 의 1.6초보다 3× margin → false positive 거의 0
- network jitter 일시적 (수백 ms ~ 수초) 은 tolerate

### 트리거 동작

[RtspDetectorUnit.cpp:1426 dequeue_wait_for(100ms)](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1426) 의 timeout 분기:

```cpp
if( !opt_item.has_value() ) {
    if( inflight_q_->is_terminated() ) break;

    // [NEW] frame-age 기반 force-reset
    if( rtsp_receiver_ ) {
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch() ).count();
        const int64_t last_ns = rtsp_receiver_->GetLastFrameNs();
        if( last_ns > 0 ) {  // 서비스 시작 직후 첫 frame 안 받았으면 skip
            const double age_sec = static_cast<double>( now_ns - last_ns ) / 1e9;
            if( age_sec > 5.0 && rtsp_client_ ) {
                // 중복 트리거 방지 — RequestReconnect 내부에 eos_pending 가드 있음
                MLOG_WARN( "CAM[%d] frame-age %.2fs > 5s — force RequestReconnect", id_, age_sec );
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_force_reset_total",
                    { { "cam_id", std::to_string( id_ ) }, { "reason", "frame_age" } } );
                rtsp_client_->RequestReconnect();
            }
        }
    }
    continue;
}
```

### 가드

1. **중복 트리거 방지**: `RequestReconnect()` 내부의 `eos_pending_` flag 확인. 이미 reset 진행 중이면 ignored (log: `RequestReconnect ignored (already pending)`)
2. **시작 직후 false positive 방지**: `last_ns == 0` (첫 frame 도 안 받음) 인 경우 skip. 서비스 시작 후 첫 frame 받기 전까지는 force-reset 안 함.
3. **Loop 빈도**: InferenceThread 의 `dequeue_wait_for(100ms)` cycle 마다 check → 검출 latency 최대 100ms.

### 새 metric

`detectbase_force_reset_total{cam_id, reason}`:
- 발생 빈도 추적
- `reason=frame_age` (이번 mitigation) / 향후 `reason=...` 추가 가능

## 검증 가능한 효과

### 회복 시나리오 측정
1. force-reset 적용 후 cam stuck 재발 시:
   - 5초 안에 force-reset 트리거
   - ResetSourceOnly 진입 → 새 TCP / 새 RTSP session
   - 외부 server 의 frame 정상 stream 시작 → 회복
2. 회복 = **B 가설 (우리 측 stale)** 확인
3. 안 회복 = **A 가설 (외부 server frozen, 새 session 도 frozen)** 확인 → 다른 mitigation 필요

### 운영 영향
- 정상 운영 중 false reset 0 발생 기대
- stuck 발생 시 ~5초 시차 + ~1.6초 reset = **~7초 안에 자체 회복**
- 지금 누적 stuck 시간 71분 → mitigation 후 **수 분 → 수 초로 99% 감소** 기대

## 미배포 사유

사용자 지시 (2026-05-20 17:50 KST): "5초로 해라. 그리고 이거 일단 두고. 문서화 해놔라."

배포 시점 결정 후:
1. 새 branch `fix/frame-age-force-reset` (develop fork)
2. 위 코드 적용 + `force_reset_total` metric register
3. cmake 0.1.9 → 0.1.10 patch +1
4. build → service restart → cam stuck 재발 까지 monitoring
5. mitigation 효과 측정 + 가설 분기 확정

## 관련 문서

- [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md) — 1차 cam stuck (05:42 KST)
- [MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md) — 별도 mismatch issue
- [BASELINE_dump_20260520_140935/](BASELINE_dump_20260520_140935/) — 정상 baseline bus_message 분포
- [stuck_dump_20260520_171959/](stuck_dump_20260520_171959/) — cam 660 stuck 시점 forensic dump

## 관련 ENV

- 운영 환경: test (MP4 파일 5분 cycle replay 형 RTSP server)
- 외부 server IP: 192.168.2.111~114 port 30000
- service: detectbase_service (PR #16+#17 진단 binary, develop @ 7b733c0 cmake 0.1.9)
- test 환경이 production 보다 가혹 → test 통과 = production 통과 ([feedback_test_env_strict.md](../.claude/projects/-home-claudedev-DetectBase/memory/feedback_test_env_strict.md))
