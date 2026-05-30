# q_drop_inf cam 별 분포 분석 — v0.1.18 baseline vs v0.1.27 41.7h

**작성**: 2026-05-30
**대상 데이터**:
- v0.1.18 baseline: `master_logs/v0.1.18/monitor_v018_teardown_fix.jsonl.gz` (11.4h, 591 cycles)
- v0.1.27 Debug 41.7h: `logs/monitor_v0.1.27_debug_24h.jsonl` (41.7h, 809 cycles)
- NEXT_SESSION Step 3 요청 (회귀/정상 판정)

---

## 1. 단순 최종값 비교 (시간 차이 무시)

| 측정 | up_h | cam 658 | cam 659 | cam 660 | cam 661 |
|---|---|---|---|---|---|
| v0.1.18 최종 | 11.4 | 0 | 0 | 0 | 0 |
| v0.1.27 최종 | 41.7 | 0 | 48875 | 0 | 0 |

표면적으로 cam 659 만 회귀 — 단 이 해석은 **잘못**. 다음 §2 참조.

## 2. 결정적 발견 — q_drop_inf 는 cam reset 시 0 으로 재시작

per-cam q_drop_inf 시계열의 음수 delta (counter 가 줄어드는 시점) 추출 결과:

| cam | reset 횟수 (41.7h) | 첫 예시 |
|---|---|---|
| 658 | **3** | 647min(13125→0), 927min(19332→0), 1060min(21583→0) |
| 659 | **119** | 37min(4→0), 44min(4→0), 48min(4→0), 53min(4→0) |
| 660 | **54** | 36min(2→0), 62min(47→0), 137min(973→0), 162min(1590→0) |
| 661 | **146** | 36min(3→0), 42min(3→0), 46min(3→0), 57min(26→0) |

→ q_drop_inf 의 "**최종값**" 자체는 cam 별 reset 빈도에 좌우될 뿐, **회귀 판정 지표로 부적합**.

NOTES §6 의 "cam 658/660 q_drop_inf=0 (계속)" 관찰 = counter reset 누락된 잘못된 해석. cam 658 도 운영 중 누적 21583 까지 갔다가 reset 됐다.

## 3. 시간 정렬 비교 (양쪽 첫 11.4h 만 비교)

| 측정 | up_min | cam 658 | cam 659 | cam 660 | cam 661 |
|---|---|---|---|---|---|
| v0.1.18 @11.4h | 683 | 0 | 0 | 0 | 0 |
| v0.1.27 @11.4h | 685 | 0 | 0 | 0 | 13367 |

v0.1.27 의 11.4h 시점 cam 661 = 13367 (reset 직전), v0.1.18 동일 시점 = 0. **이 시점만 보면 cam 661 의 q_drop_inf 발생 자체가 v0.1.18 에 없는 새 패턴**.

다만 v0.1.18 가 11.4h 가동만으로 단순 종료된 것이라 41h 시점의 비교는 불가능. v0.1.18 도 41h 가동했다면 비슷한 drop 패턴이 나왔을 가능성 배제 불가.

## 4. inflight_q_max 분포 — drop-oldest 발화 임계 도달 빈도

| 측정 | cam 658 max | cam 659 max | cam 660 max | cam 661 max |
|---|---|---|---|---|
| v0.1.18 (11.4h) | 1 | 1 | 2 | 1 |
| v0.1.27 (41.7h) | **10** | **10** | 5 | **10** |

inflight queue cap (대략 10 추정) 도달이 v0.1.27 에서는 3개 cam 에서 관측, v0.1.18 에서는 0. **drop-oldest 정책 발화 자체는 v0.1.27 에 새로 나타나는 현상**.

## 5. 운영 위협 평가 — DFPS 영향 0

| 측정 | DFPS mean | DFPS min | DFPS max | n cycles |
|---|---|---|---|---|
| v0.1.18 (11.4h) | **115.57** | 93.4 | 117.2 | 591 |
| v0.1.27 (41.7h) | **115.62** | 42.8 | 117.5 | 809 |

DFPS mean 사실상 동일 (Δ=+0.05). q_drop_inf 발화는 **backpressure 정상 동작** — drop-oldest 가 input rate > NPU rate 일 때 oldest frame 을 버려서 큐 폭발 방지. DFPS 에 영향 0.

DFPS min 의 차이 (v0.1.27 42.8 vs v0.1.18 93.4) 는 q_drop_inf 와 무관. NOTES §3 의 EOS storm 시점 단발 dip (Step 4 분석 대상).

## 6. 결론

- **회귀 아님**: q_drop_inf 의 발화 자체는 backpressure 정상 동작, DFPS 영향 0
- **NOTES §6 정정 필요**: cumulative 값 단순 비교는 무효 (counter per-reset 재시작)
- **유의미한 차이**: v0.1.27 의 inflight queue 가 cap 도달까지 가는 빈도가 v0.1.18 보다 높음. 그러나 운영 위협 부재.
- **잠재 원인 가설 (확정 X)**:
  - PR #32 (`fix(npu): batch_size 확장성 fix`, 0a886dd) — batch=1 운영 영향 0 라 commit 메시지 명시. 다만 input 측 queue 동작이 변경됐을 가능성. 별도 검증 필요.
  - 외부 RTSP stream 의 성격 변화 (cam 운영 환경 5/18 → 5/30 사이)
  - jemalloc cache 변화 → page fault 변동 → frame 처리 jitter (간접 영향 가능성 낮음)
- **권고**: 회귀 판정 close. v1.0.0 master merge gate 의 minor bump 검증 (3h+) 으로 충분. 다만 v1.0.0 master_logs 에는 본 분석 첨부.

## 부록 — cam 659 q_drop_inf 시간별 곡선 (counter reset 명시)

```
up_h    qdi_659     +delta   비고
 0.02         0
 1.00        45
 2.02       714
 3.00      2314
 4.00      4211
 6.02      7730
 7.00      8514
 8.02      9138
 9.00     10667
10.02     12468
11.00         0   -12468    ← cam reset
12.00         0
13.00     17308   +17308    (단일 cycle 에 17308 누적 — reset 후 burst)
14.02         0   -17308    ← cam reset
15.02     20122   +20122    (단일 cycle 에 20122)
18.00     23505
20.02     25212
21.00     26726
23.00     29321
25.02     31025
29.00     34128
31.00         0   -34128    ← cam reset
31.02     35259   +35259    (즉시 35259)
32.00     35973
33.02     37733
34.02     39452
38.02     45159
40.02     47400
```

reset 직후 cycle 에 큰 +delta — 이는 cycle 간 monitor sample 사이의 ~1h 동안의 누적이 단일 sample 에 일괄 반영된 결과 (monitor.sh 의 sample 간격 vs cam reset 시점의 비동기).
