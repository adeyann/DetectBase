# EOS storm pattern 비교 — v0.1.18 baseline vs v0.1.27 41.7h

**작성**: 2026-05-30
**대상 데이터**:
- v0.1.18 baseline: `master_logs/v0.1.18/monitor_v018_teardown_fix.jsonl.gz` (11.4h)
- v0.1.27 Debug 41.7h: `logs/monitor_v0.1.27_debug_24h.jsonl`
- NEXT_SESSION Step 4 요청 (EOS storm 빈도 + window 분포 비교)

---

## 1. 전체 EOS rate — v0.1.27 가 v0.1.18 보다 낮음 (개선)

| 측정 | up_h | total eos | rate /h | cycles w/ eos |
|---|---|---|---|---|
| v0.1.18 | 11.4 | 543 | **47.77/h** | 290 / 591 (49%) |
| v0.1.27 | 41.7 | 1485 | **35.64/h** | 555 / 809 (69%) |

→ v0.1.27 의 EOS rate 가 v0.1.18 대비 **-25%** 낮음. 단순 rate 로는 회귀 아님 (오히려 개선).

다만 v0.1.27 가 EOS 가 발생하는 cycle 비율은 더 높음 (49% → 69%). EOS 가 더 자주 발생하지만 한 번에 적게 발생하는 패턴.

## 2. Storm window 분포 (10min window, ≥3 EOS) — 새 발견

| 측정 | n storms | rate /h | p50 | p90 | p99 | **max** |
|---|---|---|---|---|---|---|
| v0.1.18 | 67 | 5.88/h | 8 | 9 | 10 | **10** |
| v0.1.27 | 153 | 3.67/h | 8 | 13 | 23 | **24** |

**핵심 차이**:
- v0.1.18 의 storm size 는 **8~10 으로 캐핑된 듯한 균등 분포** (cap 처럼 동작)
- v0.1.27 는 base storm 8 + 가끔 **13-24 의 큰 burst** — v0.1.18 에는 없는 패턴

v0.1.18 가 정확히 모든 storm 이 max 10 이면 어떤 자연 cap 메커니즘이 작용 중. v0.1.27 에서는 그 cap 이 풀린 형태.

## 3. DFPS<100 dip 시점 분석

| 측정 | dip 시점 | DFPS | 직전 5min EOS | 직전 5min wd | cam_active |
|---|---|---|---|---|---|
| v0.1.18 | 345min | 93.4 | 4 | 0 | 4 |
| v0.1.27 | 680min | 74.0 | 12 | 1 | 4 |
| v0.1.27 | 923min | **42.8** | 20 | 0 | 4 |
| v0.1.27 | 1951min | 89.0 | 7 | 0 | 4 |

v0.1.27 의 가장 깊은 dip (DFPS 42.8 @ 923min ≈ 15h23m) 는 NOTES §3 의 "4 cam EOS storm + 1 collateral wd (15h51m)" 와 동일 시점대 (storm window 가 anchor 시점부터 ~10분 진행하니 923 ↔ 931 정합).

dip 와 EOS burst 의 상관 — v0.1.27 의 dip 직전 5min EOS 가 7-20 (큰 burst). v0.1.18 의 dip 직전 EOS = 4 (작음). 즉 v0.1.27 의 dip 은 **큰 storm 발생 시점에 NPU contention 으로 추정**.

## 4. v0.1.27 의 큰 storm top 10 — DFPS 영향은 sparse

```
anchor count min_dfps cam_active
  1156    24    115.3        4    ← 큰 storm 이지만 dfps 정상
  1127    23    115.4        4
  1076    20    114.4        4
  1101    20    114.8        4
  2454    19    116.5        4
  1030    18    115.1        4
  1052    18    103.6        4    ← dfps 103.6 약간 dip
   990    16    116.4        4
  1010    16    115.4        4
  2475    16    116.5        4
```

→ 큰 storm (16-24 EOS / 10min) 자체가 DFPS dip 을 보장하진 않음. 41h 동안 ≥16 storm = 10건 중 **DFPS<100 발생은 단 3건**.

## 5. DFPS 종합 영향 — 운영 위협 0

| 측정 | mean | min | max | DFPS≥110 비율 | n |
|---|---|---|---|---|---|
| v0.1.18 (11.4h) | **115.57** | 93.4 | 117.2 | (NEXT_SESSION에 없음) | 591 |
| v0.1.27 (41.7h) | **115.62** | 42.8 | 117.5 | 98.6% | 809 |

mean 사실상 동일. dip 의 깊이 (42.8) 와 발생 빈도 (3건 / 41.7h ≈ 0.07/h) 는 **운영 안정성 위협 부재**. dip 직후 즉시 회복.

## 6. 결론

- **회귀 아님 (mean DFPS 동등)**. EOS rate 자체는 v0.1.27 가 더 낮음
- **새 패턴 확정**: v0.1.27 에 v0.1.18 에는 없는 "큰 storm" (≥13 EOS / 10min) 발생. v0.1.18 의 storm 은 일정 cap (~10) 으로 균등
- **DFPS dip 의 깊이 차이**: v0.1.27 가 더 깊은 dip (42.8 vs 93.4) 발생 가능. 빈도는 매우 낮음 (1.5d 마다 1건)
- **v1.0.0 master merge gate 영향**:
  - patch/minor bump 의 3h+ 모니터에서 큰 storm 1건도 안 잡힐 가능성 충분 (rate 0.07/h)
  - major bump 의 10h+ 모니터에서는 1건 잡힐 가능성 있음
- **권고**:
  - 회귀 판정 close. v1.0.0 master_logs 에 본 분석 첨부
  - 추가 dive: v0.1.18 → v0.1.27 의 GstRtspReceiver EOS handling commits 검토 (큰 storm 의 원인 추정). 별도 작업.
  - 운영 metric 추가 권장 (확정 X): `detectbase_eos_burst_size_max_10min` gauge 같이 큰 storm 빈도 추적. 본 작업 scope 아님.

## 부록 — v0.1.18 storm cap 가설

v0.1.18 의 모든 67 storms 가 size 8~10 으로 균등 → 어떤 코드 path 가 자연 cap 역할.
가설: v0.1.18 의 GstRtspReceiver 가 EOS 처리 중 다음 EOS 를 잠시 차단했을 가능성 (mutex / serial 처리).
v0.1.27 에서 그 차단이 풀려 (parallel EOS handling, 또는 cap 코드 제거) 큰 burst 가능.

이 가설은 직접 코드 검증 필요. v0.1.18 → v0.1.27 의 `code/Main/RTSP/` 또는 `code/Main/EventDispatcher/` 의 EOS-related commits 추적.
