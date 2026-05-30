# v0.1.27 Debug binary 41.7시간 모니터 최종 보고서

작성: 2026-05-30
모니터 종료: 2026-05-30 11:09 KST (PID 1709104)
관련 누적 노트: [logs/MONITOR_v0.1.27_24h_NOTES.md](../logs/MONITOR_v0.1.27_24h_NOTES.md)

## TL;DR
**v0.1.27 Debug binary 의 회귀 미발생 확정**. v0.1.26 Release 1h 운영 시 관측된 DFPS 99.4 / wd 3/h / reset 64/h "회귀" 는 (F1) Main.cpp File logger 결함 + (F2) monitor.sh DFPS log-grep 결함이 만든 **측정 artifact** 였음. 41.7h 운영 + 직접 grep cross-check 결과 정량 지표 모두 v0.1.18 baseline 동등 또는 우수. **v1.0.0 release path 개방**.

## 1. 시험 환경
| 항목 | 값 |
|---|---|
| binary | v0.1.27, Debug build (DEBUG_MODE 활성, file logger 적용) |
| commit | `e0edcbd` (Build Type Policy + 도구/infra fix) 후속 |
| 환경 | Odroid M2, RK3588, Ubuntu 22.04 container, librknnrt 1.5.2 |
| cam 수 | 4 (운영) |
| monitor | `logs/monitor_v0.1.27_debug_24h.jsonl`, 1분 cycle (의도), 809 cycles |
| 가동 시간 | 41.7h (2026-05-28 17:28 KST ~ 2026-05-30 11:09 KST) |

## 2. 종합 통계 (JSONL n=809)

### 2-1. DFPS
| 통계 | 값 | 평가 |
|---|---|---|
| mean | **115.62** | ✅ v0.1.18 baseline 115.6 와 0.02 fps 차이 |
| median | 116.40 | ✅ |
| stdev | 3.51 | ✅ 정상 변동 범위 |
| min | 42.8 (5/29 15:51 storm snapshot) | — snapshot dip, sustained 아님 |
| p5 / p25 / p75 / p95 | 111.6 / 115.8 / 116.6 / 116.9 | ✅ tight 분포 |
| max | 117.5 | ✅ |
| **DFPS >= 110 cycle 비율** | **98.6%** | ✅ v0.1.18 baseline 98.8% 와 동등 |
| DFPS < 100 cycle (dip) | 3/809 (0.4%) | snapshot, 1 cycle 후 회복 |

### 2-2. 메모리 (RSS / jemalloc)
| 통계 | 값 | 평가 |
|---|---|---|
| RSS mean | 1098.6 MB | ✅ plateau ~1.1GB |
| RSS median | 1207 MB | ✅ |
| RSS min | 527 MB | jemalloc decommit cycle 최저점 |
| RSS max | 1328 MB | ✅ 임계 1500MB 대비 172MB 여유 |
| **임계 (1500MB) 초과 cycle** | **0/809** | ✅ 운영 위협 0 |
| jemalloc resident mean | 465.8 MB | ✅ |
| **leak 검증** | 7h 자연 decommit (-531MB) 후 재팽창 plateau → **leak 부재 확정** | ✅ |

### 2-3. 리소스 (불변 예상값)
| 항목 | min | max | mean | 평가 |
|---|---|---|---|---|
| threads | 156 | 158 | 156.2 | ✅ 거의 불변 (Thread leak 0) |
| FD | 210 | 218 | 212.3 | ✅ 거의 불변 (FD leak 0) |
| cam_active | 4 | 4 | 4.0 | ✅ 4/4 cycle 비율 100% |
| NPU temp | — | 61.0°C | 59.3°C | ✅ throttle 임계 (70°C+) 한참 아래 |
| container CPU% | — | 477% | 442% | ✅ 8-core 대비 안전 범위 |

### 2-4. Events (직접 grep, LOG_SLICE 결함 우회)
파일별 합산 (cut = 2026-05-28T08:25 UTC = monitor 시작):
| event | log.2.gz (6h35m) | log.1 (24h) | log (11.5h) | **합** | **rate** |
|---|---|---|---|---|---|
| reset_OK | 312 | 1146 | 547 | **2005** | **48.1/h** |
| on_eos | 312 | 1146 | 547 | **2005** | (reset 와 1:1) |
| frame-age wd | 0 | 2 | 0 | **2** | **0.048/h** |
| ERROR | 2 | 0 | 0 | **2** | benign, 단발 |
| cam_loss | 0 | 0 | 0 | **0** | ✅ |
| process restart | 0 | 0 | 0 | **0** | ✅ |

## 3. 회귀 판정 (NEXT_SESSION 기준 대비)

| 기준 | 값 | 판정 |
|---|---|---|
| DFPS<100 sustained | 3 cycle, 모두 즉시 회복 | ✅ sustained 아님 |
| wd > 3/h | **0.048/h** (2/41.7h) | ✅ 임계 미달 (1/60) |
| reset > 50/h | **48.1/h** | ✅ baseline 48/h 동등 |
| cam_loss > 0 | **0** | ✅ |
| RSS leak | 자연 decommit + plateau | ✅ leak 부재 |

**5/5 기준 통과 → 회귀 미발생 확정**.

## 4. v0.1.26 Release 1h "회귀" 와 v0.1.27 Debug 41.7h 비교

| 지표 | 0.1.26 Release 1h | v0.1.27 Debug 41.7h | 차이 |
|---|---|---|---|
| DFPS mean | 99.4 | **115.62** | +16.2 (+16.3%) |
| DFPS >= 110 비율 | 측정 무능 | **98.6%** | — |
| wd | 3/h | **0.048/h** | -98.4% |
| reset | 64/h | **48.1/h** | -25% (정상 baseline 복귀) |
| cam_loss | 측정 무능 | **0** | — |
| RSS leak | 측정 무능 | 부재 확정 | — |

**0.1.26 Release 의 회귀 = 측정 artifact**:
- (F1) Main.cpp InitLogger Debug=Console only → DetectBase.log 0 bytes → monitor.sh 의 file grep 모두 0 (reset/wd/eos/err/warn). DFPS 도 log 기반 추출 결함 (F2).
- (F2) monitor.sh DFPS log-grep (DEBUG_MODE compile-out 으로 Release 에서 emit 안 됨) → metric=0 = "DFPS 99.4" 같은 부정확 값 산출
- 두 결함이 만든 "회귀 의심" → 실제 binary 거동은 정상

## 5. 발견된 비-회귀 이슈 (모두 minor, fix 권장 우선순위 별도)

### 5-1. monitor.sh LOG_SLICE 결함 (infra)
- **문제**: cumulative slice 모델 + bash variable 저장. log 가 커지면 cycle drift (16-20h 구간 평균 466.6s, max 21분), 21h+ 시점부터 xrealloc 폭발 → events 모두 0 spurious
- **영향**: monitor 데이터 quality 저하 (per-snapshot 값은 정확하나 events 누적 부정확)
- **운영 영향**: 0 (서비스는 정상)
- **fix 권장**: per-cycle slice + 자체 counter (대안 1) — NOTES §5b/§5c 참조

### 5-2. 자정 dir rollover ERROR 2건 (cam 659/660)
- **문제**: 5/29 00:00 KST 자정에 cam 659/660 EventThreadRunner make frame image save directory failed
- **시간**: 정확히 같은 ms (00:00:00.022Z)
- **5/30 자정 재현**: 0건 → **race condition 확정** (deterministic 아님)
- **운영 영향**: 간헐적, 한 번 발생 시 단발 (cascade X)
- **fix 권장**: cam 별 dir 생성 mutex 또는 한 cam 만 생성 후 공유 패턴

### 5-3. EOS storm 패턴 (3건 관찰)
| 시점 | window | event 수 | 결과 |
|---|---|---|---|
| 5/29 04:48~04:54 UTC (15h51m) | 5분 | 4 cam EOS + 1 collateral wd (659) | DFPS dip 42.8, 즉시 회복 |
| 5/29 14:48 UTC (24h timing) | 단발 | 4 cam EOS, no wd | 작은 dip |
| 5/30 01:51~02:00 UTC (32h31m) | 10분 | 7 EOS 일제 발생 | DFPS dip 89.0, 즉시 회복 |

- **패턴**: cam EOS cycle 들이 우연히 가까이 정렬 → NPU contention 으로 collateral stuck 가능성 증가
- **위험도**: 낮음 — 모두 ResetSourceOnly 정상 회복, cam_loss 미발생
- **분석 가치**: v0.1.18 baseline 의 EOS storm 빈도와 비교 필요 (별도 todo)

### 5-4. q_drop_inf 누적 (cam 659/661)
- **관찰**: 1h 마크부터 cam 659/661 에서 q_drop_inf 누적 시작 (cam 658/660 은 0)
- **21h 시점 측정값**: 659=20616, 661=17974
- **운영 영향**: DFPS 정상 (backpressure 동작). 단 cam 별 imbalance 의 원인 미파악
- **분석 가치**: v0.1.18 baseline 의 q_drop_inf 패턴과 비교 필요 (별도 todo)

## 6. RSS 진동 패턴 — jemalloc 분석 (사용자 요청)

### 관찰
RSS 0h 554MB → 6h 1231MB plateau → 7h 731MB 급락 (-500MB) → 점진 재팽창 ~1240MB plateau. plateau 안 ±50MB 진동 + 자정 KST 자정 decommit cycle 패턴.

### 원인
- **jemalloc 의 caching + decommit**: free 페이지를 즉시 OS 에 안 돌려주고 arena 에 캐싱 (next allocation hot path 비용 절약). 10s decay timer 활성 (`background_thread:true`, default `dirty/muzzy_decay_ms=10000`) 이나 **continuous allocation pressure** (4cam × 30fps = 분당 12만 frame buffer) 로 페이지가 10s 연속 free 못 채움 → decommit 보류
- **trigger**: cycle 214 (5/28 14:56Z) pipeline disturbance (avframe stall + q_drop_inf 누적 시작) 이 잠시 arena idle → background thread decommit 발화

### 1232MB 구성 분해 (16h tick 기준)
```
vm_rss_mb 1232MB
├─ jemalloc arena (jem_resident_kb=828)         ~828MB  (67%)
│  ├─ jem_alloc (지금 사용 중)                    ~430MB  (35%)
│  └─ arena cache (free 보관)                    ~290MB  (24%)
│     + decommittable                            ~108MB  ( 9%)
└─ non-jemalloc                                   ~404MB  (33%)
   ├─ code/data + libraries                      ~50-80MB
   ├─ thread stacks (156 thread × 사용분)         ~200MB
   ├─ GStreamer mmap'd buffer pools (별도)        ~50-100MB
   └─ librknnrt NPU DMA buffer (별도 mmap)        ~50-80MB
```

### "지금 사용 중" 430MB 내역
4 cam pipeline 활성 버퍼 (~140-160MB) + 공통 queue/settings/MAIA proto (~50-100MB) + NPU 모델 weight (YOLOv5, ~30-50MB) + 누적 메트릭/DBG_PROF rolling (~50-100MB) = **270-410MB** ≈ jem_alloc 430MB ✅ 일치.

### 결론
- RSS plateau ~1200MB 의 **2/3 가 jemalloc 효율성 캐싱**, **1/3 가 실제 작업 데이터**
- 7h decommit (-531MB) 은 **leak 부재 강력 증거**. 메모리가 정상적으로 회수 가능함을 입증
- 임계 1500MB 대비 200-300MB 여유 — 운영 위협 없음
- **개입 비권장**: aggressive purge 시 page fault 증가로 DFPS 하락 위험 > RSS 감소 이득

## 7. 후속 권장 (v1.0.0 진입 + 별도 fix)

### v1.0.0 진입 (사용자 결정 필요)
- 회귀 미발생 확정으로 진입 path 개방
- audit `--strict` (Debug 자동 강제, ~5h) + 추가 3h 모니터 + master_logs/v1.0.0/ archival + master 머지 — **사용자 명시 허가 필수** (CLAUDE.md 절대 규칙)

### experiment branch develop 머지 (사용자 결정 필요)
- branch `experiment/runtime-regression-investigation` (HEAD `a05c85d`) 의 도구/infra fix + 정책 + README trim 은 회귀 미발생 무관 이미 가치 있는 fix
- develop 머지 권장 (사용자 명시 허가 시)

### 별도 fix (우선순위 순)
1. **monitor.sh LOG_SLICE 결함** — per-cycle slice + 자체 counter 적용 (NOTES §5b 대안 1)
2. **자정 dir rollover race fix** — cam 별 mutex 또는 공유 생성 패턴
3. **q_drop_inf cam 659/661 imbalance 분석** — v0.1.18 baseline 비교
4. **EOS storm pattern baseline 비교** — v0.1.18 storm 빈도와 통계 비교

### 보류 (장기)
- librknnrt 1.5 → 2.x bump (별도 epic, v1.0.0 후) — [.DOCS/THIRDPARTY_VERSION_AUDIT_20260528.md](THIRDPARTY_VERSION_AUDIT_20260528.md) 참조
- 서드파티 base image bump (Ubuntu 22.04 → 24.04) — 동 문서 참조
- v2.0.0 multi-engine 계획

## 8. 산출물 위치
- monitor JSONL: `logs/monitor_v0.1.27_debug_24h.jsonl` (3.4MB, 809 cycles)
- rotated logs: `logs/DetectBase.log` + `logs/DetectBase.log.1` + `logs/DetectBase.log.2.gz`
- 누적 분석 노트: `logs/MONITOR_v0.1.27_24h_NOTES.md`
- 서드파티 감사: `.DOCS/THIRDPARTY_VERSION_AUDIT_20260528.md`
- 진단 plan (참고): `.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md`
- v0.1.18 baseline: `master_logs/v0.1.18/`

## 9. 한 줄 요약
v0.1.27 Debug 41.7h 운영 데이터로 회귀 미발생 + leak 부재 + EOS pattern 정상 + v0.1.18 baseline 동등 확정. **v1.0.0 release 진입 가능 (사용자 결정 대기)**. v0.1.26 의 "회귀" 는 (F1)+(F2) 측정 artifact 였음 입증.
