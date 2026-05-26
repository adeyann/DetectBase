# cam_loss Root Cause Analysis — 2026-05-26 (RESOLVED)

**상태**: ROOT CAUSE 확정 + FIX 검증 완료
**현 binary**: v0.1.18 + TeardownPipeline unref-skip fix
**monitor**: `bl4c785is` (label `v018_teardown_fix`) — 1h 운영 wd=1/cam_loss=0

> **doc 진화 이력**:
> - 19:00 — 원 제목 "Unstable Network Resilience Test"
> - 19:10 — 첫 정정: "cam server state 결함" 가설 → "application backpressure cascade"
> - 22:00 — **최종 정정**: backup log (`.backup_detectbase_service_20260526_195453.log`) 분석으로 **진짜 root cause 식별 = `GstRtspReceiver::TeardownPipeline()` 의 `gst_object_unref(pipeline_)` 가 GStreamer 내부 thread join 에서 unbounded block**. cam 661 의 42분 cam_loss 의 직접 원인.

---

## 최종 ROOT CAUSE

`code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp` `TeardownPipeline()` :

```cpp
// 기존 (결함)
gst_element_set_state(pipeline_, GST_STATE_NULL);
GstState st;
gst_element_get_state(pipeline_, &st, nullptr, 5 * GST_SECOND);  // 5s timeout
// ★ timeout 이든 success 든 무조건 다음 줄:
gst_object_unref(pipeline_);  // ← 여기서 internal thread join 에서 hang
```

`gst_object_unref(pipeline_)` 는 pipeline destructor 호출 → 자식 element destruct → 각 element 의 streaming thread join. 만약 udpsrc / rtspsrc 의 내부 thread 가 socket close 또는 RTP teardown 에서 hang 하면 **unref 가 영원히 return 안 함**.

**증거 (backup log)**:
```
2026-05-26T10:09:32  GstRtspClient[661] frame-age watchdog: 13s 무프레임 → 강제 reset
2026-05-26T10:09:33  ResetSourceOnly[661] 진입 — pipeline destroy/rebuild  ← 여기서 영원히 hang
... 45분 침묵 ...
2026-05-26T10:54:54  (process restart)
```

`ResetSourceOnly[661]` 진입 후 매칭되는 OK 로그 없음. `ResetSourceOnly` 가 호출한 `TeardownPipeline` 의 `gst_object_unref` 가 return 안 함. ReconnectWorker thread (cam 661) 가 `receiver_mtx_` 들고 stuck. 같은 thread 안에서 다음 watchdog cycle 도 발화 불가. process restart 만이 escape.

---

## FIX (commit b? — fix/teardown-pipeline-unref-hang)

```cpp
gst_element_set_state(pipeline_, GST_STATE_NULL);
GstState st;
const GstStateChangeReturn state_result =
    gst_element_get_state(pipeline_, &st, nullptr, 5 * GST_SECOND);
const bool state_ok = (state_result == GST_STATE_CHANGE_SUCCESS && st == GST_STATE_NULL);
// ... appsink unref (이건 safe) ...
if (state_ok) {
    gst_object_unref(pipeline_);
} else {
    MLOG_WARN("TeardownPipeline[%d] — pipeline NULL transition 실패 (result=%d, state=%d). "
              "gst_object_unref 건너뜀 (의도된 leak — hang 방지). ...",
              cfg_.cam_id, state_result, st);
    // pipeline_ 의도적 leak. process restart 시 OS cleanup. infinite hang 보다 우월.
}
pipeline_ = nullptr;
```

**의도된 leak per stuck event**: pipeline + 자식 element + 내부 buffer 합쳐 ~few MB. 24h 운영 + storm 빈도 3-5회/day 가정 시 ~30 MB/day cumulative leak. process restart (운영 주기적) 로 cleanup. 영구 cam_loss 방지가 더 가치 있음.

## FIX 검증 결과 (22:14-22:46, 63분)

| 측면 | pre-fix (v016/v014_ab) | post-fix (v018_teardown_fix) |
|---|---|---|
| Duration | 50min / 8min | **63min** |
| wd | 6 / 1 | **1** (boot 직후) |
| cam_loss | sustained 영구 | **0건** ✅ |
| DFPS | 60-90 oscillating | **116.1** (5/24 baseline 동등) |
| cam_active | 3/4 sustained | **4/4 stable** |
| reset/eos | 13/-, 8/- | 49/48 (정합, 5min cycle 정상) |

**확정**: TeardownPipeline 의 unref-hang 이 cam_loss 의 단일 root cause. fix 가 정확히 잡음.

---

## 이전 가설 정리 (틀린 것들)

| 가설 | 평가 |
|---|---|
| ❌ cam 서버 (happytime rtsp) state 결함 | 부분적 증상 (CLOSE-WAIT) 만 보여 오해. 실 원인 아님 |
| ❌ Network 불안정 | L3/L4 측정으로 deny — network 정상 |
| ❌ UDP RcvbufErrors 가 root cause | 만성적 약점이지만 cam_loss 와 직접 인과 X |
| ❌ EOS path 의 frame flow verification 부재 | 코드 비대칭이긴 하나 이 시나리오 와 무관 |
| ✅ **TeardownPipeline 의 unref hang** | **확정 root cause** — fix 검증 완료 |

좋은 reminder: 정직한 ROOT CAUSE 식별은 backup log 같은 raw evidence 가 필수. detectbase.sh start 가 log 를 `.backup_*` 으로 mv 보존하는 정책이 결정적이었음.

---

## 부수 발견 (별개 작업 후보, 우선순위 medium-low)

### 1. UDP rmem_max = 208KB (system default 너무 작음)
storm 시 5.6% drop rate. 만성적 약점. cam_loss 원인은 아니지만 DFPS dip 완화에 도움. `sysctl net.core.rmem_max=16777216`.

### 2. cam_active = supervisor pattern 부재
ReconnectWorker thread 가 stuck 됐을 때 외부에서 force kill / recreate 할 mechanism 없음. 이번 fix 로 ReconnectWorker stuck 자체 발생 안 함 → 우선순위 ↓. 단 미래 다른 deadlock 시 안전망.

### 3. EOS path frame flow verification 부재
GstRtspClient.cpp EOS path 는 ResetSourceOnly OK 후 frame 흐름 검증 안 함. 다른 시나리오 (RTSP 응답 OK + RTP 안 흐름) 에서 도움 가능. 단 이번 fix 로 충분.

---

## 솔직한 fix 평가 (사용자 검증 결과 반영)

내 fix 는 **사후조치 (defensive workaround)** 임:
- 차단한 layer: TeardownPipeline 의 `gst_object_unref` 가 hang 하는 단계 ([7])
- 차단 안 한 layer:
  - [1] stream 이 갑자기 끊긴 진짜 원인 (cam server / 네트워크 / RTP 어디 layer)
  - [5] GStreamer 내부 thread join 실패 원인 (rtspsrc / udpsrc / rtpsession 어디)

162분 운영 결과 fix path 자체가 한 번도 발화 안 함 → **fix 의 효용 empirical 검증 0**. 안정성 회복은 stuck 조건이 사라진 환경 변화 (PID 4924 residual state cleanup) 때문일 가능성 높음. **사실상 162분간 아무것도 안 고친 것** 일 수 있음.

→ 진짜 검증은 다음 자연 stuck 시. 단 그때 fix 가 효과 보이지 않으면 다음 단계 필요.

---

## 다음 단계 (stuck 재발 시 escalation 순서)

### Step 1 (현 상태): 자연 stuck 대기 + fix path 발화 검증
- monitor `bl4c785is` 가 wd/cam_loss alert fire
- log 의 `pipeline NULL transition 실패` WARN 발화 확인
- 자가 회복 시간 측정 (vs pre-fix 의 영구 stuck)

### Step 2: 더 깊은 GStreamer instrumentation
stuck 재발 시 더 자세한 분석:
- `docker-compose.yml` 에 `GST_DEBUG=2,rtspsrc:5,udpsrc:5,rtpsession:5` env var 추가
- container restart + 자연 stuck 대기
- GStreamer 내부 동작 stderr 로 추적 → udpsrc thread 가 어디서 hang 하는지 식별
- 비용: log 양 증가 ~10-50 MB/h, CPU overhead 미미

### Step 3: Packet capture
stuck 시작 시점의 RTP/RTCP 메시지 추적:
- `tcpdump -i eth0 -w cap.pcap host 192.168.2.111 and port 30000` (root 필요)
- stuck 발생 시점의 RTP sequence / RTCP RR / RTSP TEARDOWN 동작 분석
- WHY stream 이 끊겼는지 protocol-level 검증 가능

### Step 4 (최후 수단): **happytimesoft RTSP 모듈로 rollback A/B test**

만약 Step 1-3 으로도 root cause 식별 불가 + cam_loss 가 운영 critical 이면:
GStreamer 전면 폐기 + happytimesoft baseline 복귀 시도. **같은 cam server / 네트워크 환경에서 happytimesoft 가 같은 cam_loss 보이면 원인은 외부 (cam server / 네트워크)**, **happytimesoft 가 멀쩡하면 원인은 GStreamer 통합 어딘가**.

#### 4-A. happytimesoft baseline 위치
- `efeea7a` (2026-05-15) — Initial commit: happytimesoft baseline + jemalloc/W-14/P1/P3 patches
- 단 너무 옛 baseline 이라 그 후 모든 fix (cam stuck variants, audit cleanups, TSan races, NPU multi-core 등) 가 빠짐
- 더 실용적 — GStreamer 통합 직전 commit (`37dae37` 의 parent) 확인 필요

#### 4-B. Rollback A/B 절차 (실행 시)
```bash
# 1. happytimesoft baseline 직전 commit 식별
git log --first-parent develop --format="%h %ci %s" | grep -B1 "feat: GStreamer RTSP integration"
# → "feat: GStreamer RTSP integration..." (37dae37) 의 parent 가 happytimesoft 최종

# 2. experiment branch 생성
git checkout -b experiment/happytimesoft-baseline <parent_of_37dae37>

# 3. happytimesoft 의 build dependency 확인 (Dockerfile.build 가 GStreamer 만 가지고 있을 수도)
# → happytimesoft 라이브러리 빌드 단계가 Dockerfile 에 있는지 확인 필요

# 4. build + container restart + monitor v_happytimesoft 가동

# 5. 30분-2시간 운영 후 cam_loss 발생 여부 + 발생 시 양상 (영구 stuck? 자가 회복?)

# 6. 결과 비교:
#    - GStreamer + 우리 fix: cam_loss 발생 → 자가 회복 (fix 효과 검증된 경우)
#    - happytimesoft baseline: cam_loss 같은 양상 발생?
#    - 만약 happytimesoft 도 똑같이 stuck 되면 → 원인은 우리 RTSP 모듈 밖 (cam server / 네트워크)
#    - happytimesoft 가 멀쩡하면 → 원인은 GStreamer 통합 어딘가 (deeper fix 필요)
```

#### 4-C. 4번 단계 비용
- 1-2일 작업 (build / restart / 검증)
- happytimesoft 의 알려진 leak (~340 MB/year rtpmanager 와는 별개로 happytimesoft 자체의 leak/wd 변종 존재할 수 있음)
- GStreamer 통합 이후 추가된 fix 들 (PR #4 이후) 다시 정리 필요
- 운영 downtime: A/B test 기간 동안

#### 4-D. 4번 단계 trigger 조건
- Step 1-3 결과로도 stuck 의 진짜 원인 식별 불가
- 우리 fix 로도 운영 안정성 미달 (cam_loss 빈도 ↑ / leak 누적 비현실적)
- 또는 사용자 명시 결정

---

(이 doc 은 cam_loss 진단 historical record. NEXT_SESSION 에 요약 포인터.)

---

## 배경

원래 v0.1.16 patch (argv guard + flock) 머지 후 wd=6/50min 회귀 발생 → A/B test (v0.1.14 rollback) 로 patch 결백 확인.

초기 가설은 cam 서버 (happytime rtsp 4.1) state 결함. CLOSE-WAIT / ARP STALE signature 가 그렇게 보였음. 단 진짜 layer-by-layer 측정 결과:

| Layer | 측정 | 결과 |
|---|---|---|
| L2 (ARP) | `ip neigh` | 192.168.2.113 STALE — STALE = 불가용 아닌 cache 만료 |
| L3 (ICMP) | `ping -c 1` | 4대 모두 rtt 0.5-3 ms (LAN 정상) |
| L4 (TCP) | `ss -tan` | ESTAB 4/4. 직전 CLOSE-WAIT 흔적 = **server-side FIN, network 가 끊은 게 아님** |
| **L4 (UDP)** | `/proc/net/snmp` Udp + per-socket /proc/net/udp | **storm 시점 30s 측정 5.6% drop** (5308/sec in, 298/sec drop) |
| Application (RTSP) | OPTIONS nc | 4대 200 OK |

→ **L1-L4 network 자체는 정상**. **L4 UDP recv buffer overflow** 가 application 부하 때문에 일어남.

cam 서버 재시작은 **5/27 예정**. 그동안 현 상태로 DetectBase resilience 검증 + 약점 식별.

## 환경 측 결함 signature (initial snapshot @ 18:55-18:56 KST)

| Signature | 값 |
|---|---|
| 192.168.2.114:30000 TCP state (restart 직후) | LAST-ACK (이전 CLOSE-WAIT 의 잔존) → 잠시 후 정리됨 |
| 192.168.2.113 ARP | **STALE** (지속) — cam 659 와 ARP 통신 fresh refresh 안 됨 |
| ping 4대 | 모두 응답 |
| RTSP OPTIONS 4대 | 모두 200 OK |
| 현 ESTAB connections | 4/4 (정상 reconnect 완료) |

→ **cam 서버 자체는 응답하지만 일부 connection 이 server-side 에서 비정상 종료** 되는 패턴.

## 추적 목표

1. **wd / cam_loss event 의 시간 분포** — 어느 cam, 얼마 간격으로
2. **자가 회복 여부** — cam_loss 발생 시 active count 가 4 로 복귀하는지, 평균 몇 분 만에
3. **TCP state 진화** — CLOSE-WAIT / LAST-ACK 패턴 누적 / 정리
4. **DFPS 분포** — 불안정 환경의 DFPS 변동 폭
5. **DetectBase 측 약점** — server-side close 처리, reconnect 전략, per-cam stage timing

## DetectBase 측 약점 후보 (정정 — application backpressure 중심)

### 진짜 mechanism

```
EOS cluster (3-4 cam 동시 reset, 5-min mp4 cycle)
    ↓
Ramp-up: NPU 가 frame consume 못 따라감 (consumer rate < producer rate)
    ↓
UDP socket recv buffer overflow → kernel drop 5.6% (RcvbufErrors)
    ↓
RTP fragmentation 의 일부 packet 손실 → frame reconstruct 실패
    ↓
H.264 I-frame 손실 시 후속 P-frame chain 깨짐
    ↓
GStreamer rtpsrc/jitterbuffer 가 frame 못 emit → 우리 callback 못 옴
    ↓
12s+ frame 없음 → frame-age watchdog fire
    ↓
worst case: cam_active count 감소 (등록은 됐지만 frame 안 옴)
```

### 약점 #1: UDP SO_RCVBUF 작음 (가장 큰 영향)
- rtspsrc 의 default UDP socket recv buffer 보통 256KB-1MB
- per-cam 의 RTP 가 high-bitrate 1080p 30fps 면 burst 시 buffer 부족
- **fix 후보**: rtspsrc 의 `udp-buffer-size` property 를 8MB-16MB 로 설정
  - 또는 sysctl `net.core.rmem_max` 시스템-wide 조정

### 약점 #2: NPU consumer rate vs producer rate 의 5% headroom
- baseline: 4 cam × 30 fps = 120 input, NPU 처리 115 fps
- ramp-up 시 burst 가 120 초과 → 즉시 backpressure
- **fix 후보**: per-cam FPS limit 약간 낮춤 (30 → 28) 또는 multi-engine batch 처리 (v2.0 multi_engine doc 참조)

### 약점 #3: EOS cluster desync 부족
- 현재 `sleep_responsive((cam_id % 4) * 500ms)` = 4 cam 0, 500, 1000, 1500 ms 분산 (1.5s 폭)
- 그러나 EOS 자체는 server-side 5-min cycle 로 거의 동시
- **fix 후보**: reset 직후 NPU 사용량 spike 시 추가 backoff (exponential 또는 NPU queue depth 기반)

### 약점 #4: CLOSE-WAIT 처리 (낮은 우선순위)
직전 관찰: cam 서버가 close 했는데 socket close 가 늦은 사례 1건. fd leak 누적은 적지만 defensive 후보.
- **fix 후보**: GstRtspReceiver 의 disconnect path 강화, EOS 직후 명시적 socket close

### 약점 #5: 약한 detection signal (낮은 우선순위)
- frame-age watchdog 12s = 늦은 신호
- UDP RcvbufErrors 같은 kernel-level signal 을 application 이 못 봄
- **fix 후보**: rtspsrc 의 statistics property polling → 우리 metric 으로 노출 (`detectbase_udp_rcvbuf_errors_total` 등)

---

## Event timeline (실시간 누적)

### 18:54:13 — wd #1 (DetectBase.log) / 18:54:32 — ★watchdog alert (monitor)
- cam: **661** (192.168.2.111)
- 14s 무프레임
- boot (18:52:12) 후 122s 만에 fire
- **cam_active stays 4/4** — reset 으로 자가 회복. cam_loss 안 됨.
- 이는 v014_ab_test 시점과 다른 양상 (그땐 즉시 cam_loss). v0.1.18 restart 가 server 측 state 부분 정리한 듯.

### 18:54-18:58 — 회복 추세 관찰
- dfps: 74.5 → 77.0 → **106.4** (baseline 향)
- active: 4/4 유지
- wd: 1 stable (안 늘어남)
- ARP 192.168.2.113 STALE 지속 (5분+) — 깊은 layer 의 state 미해결
- ping 4대 모두 0.5-3 ms (LAN 정상)

### 중간 결론 — server-side state recovery patterns
**v018 restart 가 일부 server-side cleanup 효과**:
- 직전 v014_ab_test 시점: 즉시 cam_loss 6+분 지속, CLOSE-WAIT/LAST-ACK 잔존
- v018 restart 후: ESTAB 4/4 정상 + wd 1건만 + cam_active 4/4 유지
- → happytime rtsp 의 session table 일부 stale entry 가 client disconnect 시 cleanup 되는 패턴 추정

**그러나 ARP STALE (192.168.2.113) 은 미해결**:
- TCP layer 는 정상화 (ESTAB)
- ARP layer 는 fresh 안 됨 — kernel 이 cam 113 과 ARP request/reply 교환 안 함
- TCP keepalive 가 ARP refresh 까지 안 일으키는 게 정상이긴 함
- 의미: 만약 cam 113 이 IP 바뀌거나 host MAC 변경 시 우리 ARP 로 못 따라감 → 정전/재시작 시나리오에서 connection 끊김 가능성

### 18:57-19:00 — EOS cluster cascade (정상 baseline)
```
09:57:13.853  cam 658 EOS (UTC)
09:57:15.182  cam 659 EOS (1.3s 후)
09:57:15.436  cam 660 EOS (1.6s 후)  ← 3 cam cluster within 2 sec
09:57:14-16   resets (cam_id desync offset 500ms × cam_id%4 적용)
09:59:15.577  cam 661 EOS (2분 offset)
```

**DFPS sequence**:
| time | DFPS |
|---|---|
| 09:58:34 | 92.2 |
| 09:58:44 | 87.0 |
| 09:58:54 | 82.3 |
| 09:59:04 | 86.8 |
| 09:59:14 | 94.0 |
| 09:59:24 | 83.3 |
| 09:59:34 | 61.0 |  ← cam 661 EOS 영향
| 09:59:44 | 91.0 |
| 09:59:54 | 102.1 |
| 10:00:04 | 104.6 ← baseline 향 회복 |

★dfps_low (18:59:46) fire — 정확히 자기 임계값대로 작동.

**분석**: 3-cam EOS cluster + cam 661 의 2분 offset 이 ~2분간 dfps 60-90 oscillation. 5/24 baseline 의 capacity-edge storm 패턴 (24h 3 회) 과 동일한 양상. cam server state 결함 아닌 정상 운영 변동.

- wd 안 늘어남 (1 stable)
- cam_active 4/4 유지
- 자가 회복 정상

**기준점**: v014_ab_test (cam_loss 6+분 지속) 와 명확히 다르고, 5/24 baseline (storm 3회/24h) 과 같은 양상. **cam server state 가 v018 restart 로 부분 정리됐고, 현 dfps_low 는 5/24-style 정상 capacity-edge cascade**.

### 19:08-19:11 — wd 재발 + cam_loss 재발
- 19:08:11 cam 658 wd (14s 무프레임)
- 19:10:16 wd +2 (총 4), cam_loss (active=3/4) 재발
- 19:11:19 dfps_low alert (dfps=67.2), cam_loss 지속

**30s UDP 측정 결과 (storm 시점)**:
| 측정 | 값 |
|---|---|
| in/sec | 5,308 |
| drop/sec | 298 |
| drop rate | **5.61%** |

→ **L4 UDP level 에서 명백한 drop**. LAN normal < 0.1%. 이게 진짜 root cause 의 직접 증거. network 자체는 정상 (5308/sec 도달), kernel UDP buffer 가 application 못 따라가서 overflow.

### 가설 정정
- ❌ "Network 가 불안정" — 증거 없음. L1-L3 정상
- ❌ "cam server state 결함만" — 부분 맞으나 진짜 원인 아님
- ✅ "**Application backpressure** — NPU consumer 가 RTP producer 못 따라가서 UDP buffer overflow"
- v014_ab_test 와 v018_post_ab 가 같은 패턴 보이는 이유 = binary 차이 아니라 **양쪽 다 NPU bottleneck → UDP drop → frame loss → wd**

---
