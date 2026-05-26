# Cam 659 Stuck 분석 (2026-05-20)

## Summary
cam 659 가 8시간 운영 (05-19 13:13 ~ 05-20 05:42 KST) 후 stuck. TCP ESTAB connection 유지하지만 RTP frame 0건 + reset_cnt freeze. 외부 RTSP server 정상 (별도 gst-launch client 로 확인). **DetectBase 측 `GstRtspClient[659]` internal stale state**.

## Timeline (UTC, KST = UTC+9)

| 시각 (UTC) | KST | 사건 |
|---|---|---|
| 13:13:01 | 22:13 | service 시작, 4 cam 등록 (PR #12 후 새 mount /<id>) |
| 19:14:21 ~ 20:39:44 | 04:14 ~ 05:39 | cam 659 의 5분 cycle EOS reset OK 정상 (18 회 누적, reset_cnt 11 → 79) |
| **20:39:44** | **05:39** | **cam 659 마지막 정상 EOS in-place reset OK** |
| 20:39:44 ~ 20:42:01 | 05:39 ~ 05:42 | 2분 18초간 정상 frame 흐름 (INF/RSP 100-cycle log) |
| **~20:42:01** | **~05:42** | **cam 659 frame 멈춤 (마지막 cycle log)** |
| 20:42:01 이후 5분 cycle | | cam 659 의 다음 예정 EOS (~20:44:44) — 들어오지 않음 |
| 20:52:26 | 05:52 | 첫 `Cam[659] decoded frame not recieved` warning (10분 timeout) |
| 21:00 ~ 23:24 | 06:00 ~ 08:24 | cam 659 down 지속. 다른 3 cam 정상 |

## URL/IP 매핑 (log 의 `GstRtspClient[N] 생성 — url=...` 메시지)

| cam ID | URL |
|---|---|
| 658 | `rtsp://192.168.2.114:30000/../../CAM/004.mp4` |
| **659** | **`rtsp://192.168.2.113:30000/../../CAM/003.mp4`** |
| 660 | `rtsp://192.168.2.112:30000/../../CAM/002.mp4` |
| 661 | `rtsp://192.168.2.111:30000/../../CAM/001.mp4` |

## 외부 server 검증 (down 상태에서)

| 검증 | 결과 |
|---|---|
| Ping 192.168.2.113 | alive |
| TCP port 30000 | OK |
| ESTAB connection | 4개 (cam 659 포함) 유지 |
| Standalone `gst-launch rtspsrc location=rtsp://192.168.2.113:30000/../../CAM/003.mp4` | **SDP 정상 수신, H264 video caps + ONVIF metadata + audio stream 모두 정상** |
| 다른 cam (660, 192.168.2.112) 같은 URL pattern | 동일 정상 응답 |

**결론**: 외부 server 살아있고 정상 stream 보냄. DetectBase 측 stuck.

## 정상 cam 과 차이

| 항목 | cam 658/660/661 | cam 659 |
|---|---|---|
| reset_cnt 누적 (분석 시점) | 110 / 109 / 109 (계속 증가) | **79 (frozen at 20:39:44)** |
| 5분 cycle EOS in-place reset OK log | 계속 발생 | **20:39:44 이후 0건** |
| INF/RSP 100-cycle log | 계속 발생 | **20:42:01 이후 0건** |
| events_total per cam (30분 window) | +800~3000 | **+0 / +0** |

## stuck 패턴 의문

- **EOS 시점 stuck 아님**: 마지막 EOS reset (20:39:44) 정상 처리 후 2분 18초 동안 정상 운영. EOS 와 EOS 사이의 정상 stream 흐름 중 frame 만 멈춤.
- **다음 EOS 도착 안 함**: 5분 cycle 의 다음 예정 EOS (~20:44:44) 가 bus message 로 안 들어옴.
- **TCP ESTAB 유지**: socket 자체 close 안 됨. RTP packet 만 안 받음.

## 가능한 root cause 가설

1. **외부 server 의 cam 659 stream 만 frozen** — RTP stream stop 했지만 RTSP EOS 안 보냄. TCP keep-alive 만 응답. (별 가능성, 단독 client 로 정상 stream 받았으니 다른 client 가 attach 한 후 새 session 일 가능성)
2. **DetectBase 의 rtspsrc internal stale state** — UDP RTP packet drop 또는 socket 측 stuck. EOS 인식 못 함.
3. **5분 cycle EOS 발생했지만 bus message 가 cam 659 path 에서 lost** — TSan 같은 race 가능성.

## 진단 도구 추가 (PR #16, 2026-05-20)

`debug/gst-rtsp-stale-trace` branch → develop 머지. cmake VERSION 0.1.5.

### 추가된 로그
- 모든 GstBus message type capture (`OnBusMessage` WARNING/STATE_CHANGED/STREAM_STATUS/default branch)
- 콜백 호출 직전 trigger log (on_eos/on_error/on_timeout)
- `RequestReconnect` state 변경 log (pending set / ignored)
- `ReconnectWorker` wake up 시 eos_pending state log
- `ResetSourceOnly` 진입/완료/실패 모든 path + duration

### 추가된 metric
- `detectbase_gst_rtsp_bus_message_total{cam_id, type}` — bus message type 별 카운트
- `detectbase_gst_rtsp_reset_attempt_total{cam_id, result}` — reset 시도 결과 (enter/ok/build_fail/playing_fail/no_pipeline)
- `detectbase_gst_rtsp_last_frame_age_sec{cam_id}` — last frame 부터 경과 시간 gauge (InferenceThread timeout 마다 update)

## 부수 발견 (PR #9 결함, PR #17 에서 fix)

`detectbase_correlation_mismatch_total` 가 IncrementCounter 만 호출되고 `RegisterCounter` 누락 → `/metrics` 노출 안 됨 + measure 항상 0. log 직접 count 결과 실제는 658:1290 / 659:1263 / 660:976 / 661:1353 발생. PR #17 에서 `std::call_once + RegisterCounter` 추가.

## 부수 발견 (PR #16+#17 binary 의 부작용, 별도 추적)

PR #17 binary restart 후 mismatch 폭증 (~70×, T+60 부터 stable plateau). 모든 cam, 모든 mismatch 의 delta = **정확히 10 frame** (cam 내부 NPU response stable backlog). 진단 binary 의 cache line contention 이 InferenceThread push 시간 증가 → result_q backlog 형성 추정. cam stuck 과 다른 issue (시간 패턴 다름, 운영 영향 다름). 자세한 분석: [MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md).

## 다음 stuck 재발 시 확인 항목

1. `detectbase_gst_rtsp_bus_message_total{cam_id=stuck, type=eos}` 의 증가 여부:
   - **증가 멈춤** → 외부 측 stream 자체 frozen (EOS 안 보냄)
   - **계속 증가** → 콜백 호출 path 문제 (다음 단계: reset_attempt 카운트)
2. `detectbase_gst_rtsp_last_frame_age_sec{cam_id=stuck}` 의 monotonic increase
3. `detectbase_gst_rtsp_bus_message_total{cam_id=stuck, type=*}` 의 다른 type 패턴 (정상 cam 과 비교)
4. 로그: `GstRtspClient[N] on_eos trigger`, `RequestReconnect ignored (already pending)`, `ReconnectWorker wake` 의 순서/누락 여부

## Action 결정사항

- **자동 복구 (service restart, watchdog)**: 사용자 정책상 보류 — root cause 식별 전 자동 fix 금지
- **다음 단계**: stuck 재발 까지 monitoring 운영, 발생 시 위 metric 으로 가설 좁히기
