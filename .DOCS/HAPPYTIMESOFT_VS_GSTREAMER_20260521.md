# happytimesoft vs GStreamer — cam stuck 가설 검증 리포트 (2026-05-21)

## 목적

cam stuck (RTP frame 멈춤 + EOS 안 옴 + TCP ESTAB 유지) 의 root cause 가
**GStreamer 도입과 함께 발생한 문제인지** vs **외부 server 공통 문제인지** 확정.

사용자 증언: happytimesoft (이전 maia 프로젝트) 에서는 이 현상을 본 적이 없음.

## 방법

- happytimesoft 마지막 stable commit `1fc384f` 에서 branch (`experiment/happytimesoft-rtsp-test`)
- GStreamer 측 PR #16 과 동등한 진단 metric 추가 (frame_recv / reconnect per cam)
- **동일 환경**: 같은 외부 RTSP server (192.168.2.111~114:30000), 같은 4 cam, 같은 NPU/tracker/event pipeline
- 빌드: `detectbase:before-gst-integration-20260514` 이미지 (1fc384f Dockerfile 매칭)
- 16시간 연속 monitoring (30분 cycle)

## 결과 — 16시간 운영 비교

| 항목 | GStreamer (PR#16+#17 binary, 12h) | happytimesoft (1fc384f, 16h) |
|---|---|---|
| **cam stuck 발생** | **2회** (05:42 단발 + 17:05~17:37 cascading 3 cam) | **0회** |
| 최대 동시 stuck | 3/4 cam (DFPS 116→29) | 0/4 (DFPS 항상 ~48) |
| reconnect 누적 | ~144 (5min EOS cycle, 매 cycle full rebuild) | **3** (658:1, 659:2 — 모두 자체 회복) |
| reconnect 후 회복 | ❌ 일부 stuck 진입 | ✅ 3/3 즉시 회복 |
| Threads | ~156 (5min 마다 ~32 churn) | **41** (안정) |
| RSS 증가 | +180 MB (점진) | **-2 MB** (오히려 감소, jemalloc 회수) |
| HWM peak | 1,003,908 KB | 574,012 KB (**1.75× 낮음**) |
| mismatch | 2,179,090+ (~70× surge) | **0** (없음) |
| frame_rx cam 간 variance | 큼 (cam 별 backlog 차이) | **≤ 227 (0.013%)** 매우 균등 |
| log_WARN (1회 run) | 2,490,000+ | **10** |
| 총 frame 처리 | n/a | **6,735,984** (4 cam, 16h) |

## reconnect 발생 + 회복 (happytimesoft)

| 시각 (UTC) | KST | cam | 결과 |
|---|---|---|---|
| 2026-05-20 22:54:02 | 07:54 | 659 | reconnectAfterFreeConnect → 즉시 회복 (frame 계속) |
| 2026-05-21 00:10:43 | 09:10 | 658 | 동일 회복 |
| 2026-05-21 01:20:44 | 10:20 | 659 | 동일 회복 |

→ **외부 server 의 일시 disconnect 는 happytimesoft 환경에서도 발생**. 그러나 happytimesoft 의
`reconnectAfterFreeConnect()` 가 자체 timeout 검출 → 재연결 → 즉시 회복.

## 결론

### 가설 검증: ✅ 확정

**cam stuck 의 원인은 GStreamer 측에 있다.**

근거:
1. **같은 외부 server, 같은 cam, 같은 시간** 에서 GStreamer 만 stuck, happytimesoft 는 0회
2. 외부 server 일시 끊김은 **양쪽 모두 발생** (happytimesoft 도 3회 reconnect)
3. 차이는 **회복 메커니즘**:
   - happytimesoft: 자체 timeout → `reconnectAfterFreeConnect` → 회복
   - GStreamer: EOS 안 오면 reset trigger 자체가 없음 → 무한 stuck

### GStreamer 의 결함 (구체)

1. **stuck detection 부재**: mid-stream 에서 frame 멈춰도 (EOS 없이) 검출하는 메커니즘 없음.
   현재는 EOS message 만 reset trigger. 외부 server 가 EOS 안 보내고 RTP 만 멈추면 영원히 대기.
2. **reset trigger 가 EOS 에만 의존**: happytimesoft 는 RTP/RTCP timeout 자체 검출. GStreamer 는
   `GstRTSPSrcTimeout` (RTCP) 에 의존하나, TCP keepalive 응답하는 server frozen case 미검출.

### 부수 확인 (GStreamer 의 과도한 부담)

stuck 과 별개로, GStreamer 의 5min EOS full rebuild 가:
- Thread churn 4× (156 vs 41)
- HWM 1.75× (1004MB vs 574MB)
- rtpmanager leak (happytimesoft 에는 없음)
- mismatch surge (happytimesoft 에는 없음)

→ GStreamer 사용법이 이 use case 에 전반적으로 무겁고 불안정.

## 다음 단계 옵션

### A. happytimesoft 복귀
- 이미 동작 확인 (16h 안정)
- DFPS 는 1fc384f baseline (48) — B3/B4 dfps optimization 재적용 필요
- 가장 확실한 안정성

### B. GStreamer stuck detection 추가 (mitigation)
- frame-age 기반 force-reset ([FORCE_RESET_DESIGN_20260520.md](FORCE_RESET_DESIGN_20260520.md), 5초 임계)
- happytimesoft 의 reconnectAfterFreeConnect 과 동등 메커니즘을 GStreamer 측에 구현
- GStreamer 유지하면서 stuck 만 해결

### C. GStreamer 근본 재검토
- EOS full rebuild → 경량 reset (seek/flush)
- multi-stream 처리 재설계
- GStreamer version upgrade (1.24+)
- 비용 큼

## 관련 자료

- branch: `experiment/happytimesoft-rtsp-test` (commit `2b0ce9c` instrumentation)
- monitor log: `logs/sanity_happytimesoft_*.log`
- GStreamer stuck 분석: [STUCK_ANALYSIS_cam659_20260520.md](STUCK_ANALYSIS_cam659_20260520.md)
- mismatch surge: [MISMATCH_SURGE_ANALYSIS_20260520.md](MISMATCH_SURGE_ANALYSIS_20260520.md)
- force-reset 설계: [FORCE_RESET_DESIGN_20260520.md](FORCE_RESET_DESIGN_20260520.md)
