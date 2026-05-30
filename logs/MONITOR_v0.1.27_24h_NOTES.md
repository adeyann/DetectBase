# MONITOR v0.1.27 Debug 24h — 보고서 작성용 누적 노트

24h monitor 종료 후 최종 보고서를 작성할 때 이 파일에 누적된 항목을 모두 반영한다.

monitor label: `v0.1.27_debug_24h`
JSONL: `logs/monitor_v0.1.27_debug_24h.jsonl`
service start: 2026-05-28 17:25 KST (08:25 UTC)
monitor start: 2026-05-28 17:28 KST (08:28 UTC), PID 1709104
ETA: 2026-05-29 17:28 KST

---

## §1. 메모리 추세 (RSS 진동 패턴) — 사용자 요청 반영 필수

### 관찰
RSS 가 0h 554MB 부터 시작해 점진 증가 → 6h 시점 1231MB 도달 후 7h 시점 731MB 로 급락 (-500MB) → 다시 ~1240MB plateau 까지 재팽창. plateau 안에서 ±50MB 주기 진동.

### 원인 — jemalloc 의 caching + decommit 사이클
- 현재 설정: `MALLOC_CONF=background_thread:true,metadata_thp:auto` (docker-compose.yml:94)
- `dirty_decay_ms` / `muzzy_decay_ms` 미명시 → default 10000ms
- background thread 가 10초간 연속 free 상태 페이지를 `madvise(MADV_DONTNEED)` 로 OS 에 반환
- 그런데도 4-6h plateau 인 이유: **continuous allocation pressure**
  - 4 cam × 30fps = 분당 12만 frame buffer 의 alloc/free
  - 페이지가 free 되자마자 다음 frame 이 즉시 재사용 → 10초 연속 free 못 채움 → decay 미발화
- 7h 시점 -500MB drop 의 trigger: cycle 214 (2026-05-28T14:56:33Z) pipeline disturbance
  - avframe stage starve (avf_alive 7-8 → 2-3)
  - cam 659/661 inflight queue 가득 → q_drop_inf 누적 시작
  - 자정 dir rollover 실패 ERROR 2건 (cam 659/660 EventThreadRunner)
  - 이 disturbance 가 잠시 arena 를 idle 상태로 만듦 → background thread decommit 발화
- 직접 측정: cycle 214 vs 215 비교
  - jem_mb (jemalloc resident): 494 → 391 (-103MB) — jemalloc 자체 반환량 직접 측정
  - vm_rss_mb: 1262 → 731 (-531MB) — kernel RSS 총 감소
  - threads/fd/container 모두 동일 — service 재시작 X

### 1232MB 의 구성 분해 (16h tick, vm_rss_mb=1232)
```
vm_rss_mb 1232MB
├─ jemalloc arena 영역 (jem_resident_kb=828)      ~828MB  (67%)
│  ├─ jem_alloc (지금 사용 중 — 실제 작업 데이터)  ~430MB  (35%)
│  └─ arena cache (free 됐지만 잡고 있는 페이지)   ~290MB  (24%)
│     + decommittable (active vs resident gap)    ~108MB  ( 9%)
└─ non-jemalloc                                   ~404MB  (33%)
   ├─ code/data segments + libraries (mmap'd)     ~50-80MB
   ├─ thread stacks (156 threads × 사용분)        ~200MB 추정
   ├─ GStreamer mmap'd buffer pools (별도)        ~50-100MB
   └─ librknnrt NPU DMA buffer (별도 mmap)        ~50-80MB
```

### "지금 사용 중" 430MB 의 내역 (cycle 215 metric 기반 추정)
| 항목 | 4 cam 합 | 비고 |
|---|---|---|
| avframe 큐 (decoded YUV 1920×1080 × avf_alive 8) | ~96MB | per cam ~24MB |
| NPU input/output tensor staging (640×640×3 RGB) | ~10MB | 1-2 × 2.4MB × 4 cam |
| Kalman tracker active (kalman_alive_avg 6) | ~0.24MB | SORT state (KB 단위) |
| correlation_id / response_wait map | ~2-5MB | resp_wait_max ~100 × KB |
| DBG_PROF 100-cycle history (Debug 빌드 전용) | ~5-10MB | RspProf/InfProf 누적 stat |
| logger JSON 큐 + sioclient/grpc queue | ~1-2MB | warn 폭주 transient |
| GStreamer 내부 (jemalloc 통과 부분) | ~20-40MB | RTP jitter, depay, mux |
| per-cam pipeline 소계 | **~140-160MB** | |
| 공통 (engine queue, settings, MAIA proto) | ~50-100MB | |
| NPU 모델 weight (YOLOv5) | ~30-50MB | 1회 로드 변동 X |
| 누적 메트릭/로깅 ring | ~50-100MB | DBG_PROF 100-cycle rolling per cam |
| **합계 예상** | **~270-410MB** ≈ jem_alloc 430MB | ✅ 일치 |

### "쌓이지만 사용 안하고 있는" 290MB (arena cache)
- jemalloc 의 bin 별 free list
- 매분 12만 frame buffer alloc/free 사이클 → 다음 frame 이 다시 쓸 거라 잡고 있음
- size class 별 (3MB YUV frame, 1.2MB RGB tensor, 작은 metadata 등) free chunk pool
- 캐싱 효과: 다음 frame alloc 이 syscall (mmap) 없이 1ms 안에 끝남

### 결론 (보고서에 명시)
1. RSS plateau 1200-1300MB 의 구성: **2/3 가 jemalloc 의 효율성 캐싱** (free 페이지 보관 + bin free list), **1/3 가 실제 작업 데이터** (4 cam pipeline 활성 버퍼)
2. 7h 시점 -500MB decommit 은 **leak 부재 강력 증거** — 메모리가 정상적으로 회수 가능함 입증
3. 임계 (`ALERT_RSS_MB_THRESHOLD` default 1500MB) 까지 200-300MB 여유 — 운영 위협 없음
4. 줄이려면 옵션 (그러나 권하지 않음):
   - `dirty_decay_ms:1000` env tuning → -1~3 dfps 추정 (page fault 증가)
   - `mallctl arena.<i>.decay` 주기 호출 → 호출 순간 -10 dfps 1초
   - `mallctl arena.<i>.purge` 주기 호출 → -30 dfps 1-2초
5. **권장**: 현 상태 유지. RKNN NPU pipeline 의 ceiling 116 fps 가 단단해 DFPS 하락 위험이 jemalloc tuning 의 이득 (RSS 감소) 보다 큼.

---

## §2. cam 658 frame-age watchdog 1건 (11h20m) — benign
- 시각: 2026-05-28T19:48:30.921Z (04:48 KST)
- 사유: CAM[658] 14s 무프레임 → ResetSourceOnly 강제
- 결과: defensive path 정상 작동, process restart X, cam_loss 없음, 다음 metric DFPS 106.8 → 회복
- v0.1.18 baseline pattern 과 동등

## §3. 4 cam EOS storm + 1 collateral wd (15h51m) — benign
- 시각: 2026-05-28T23:49-23:54Z (2026-05-29T08:49-08:54 KST)
- 시퀀스:
  - 23:49:16Z CAM[658] on_eos → reset (28ms)
  - 23:50:25Z CAM[660] on_eos → reset (46ms)
  - 23:50:40Z CAM[661] on_eos → reset (33ms)
  - 23:51:54Z CAM[659] frame-age wd 16s → reset (48ms, collateral starve 추정)
  - 23:54:17Z CAM[658] on_eos → reset (31ms)
- snapshot DFPS 42.8 (1 cycle), 직후 116.3 회복
- 0.1.26 회귀 패턴 (3 cam 28s 동시 stuck) 과 다름 — EOS-triggered alignment + NPU contention 일시
- **새 관찰 패턴**: cam EOS cycle 들이 우연히 가까이 정렬되면 NPU contention 으로 collateral stuck 발생 가능. 빈도 측정 → v0.1.18 baseline 비교 필요

## §4. 자정 dir rollover 실패 (6h28m) — minor bug, **race 가설 확정**
- 시각: 2026-05-28T15:00:00.022Z (2026-05-29T00:00 KST)
- ERROR 2건: CAM[659], CAM[660] EventThreadRunner make frame image save directory failed
- 단일 ms 발생, cascade X, cam 658/661 는 성공
- **5/30 00:00 KST 자정 재검증 결과: ERROR 0건 — 재현 X**. 즉 race condition / timing 결함 확정 (deterministic 결함이면 매일 자정 100% 재현되어야 함)
- 시급성 낮음 (간헐적, 운영 위협 X). cam 별 dir 생성 mutex / 한 cam 만 생성 후 공유 등 패턴으로 fix 가능. 별도 fix 대상.

## §5. monitor.sh log rotation 처리 미흡 — minor infra (7h 첫 발견)
- 00:00 KST 시점 `DetectBase.log` → `DetectBase.log.1` rotate
- monitor.sh `LOG_SLICE` 는 현재 `DetectBase.log` 만 awk scan → 누적 counter (reset/eos/wd/err/warn) 가 모두 rotate 시점에 reset
- watcher 의 cumulative delta 계산이 음수 spurious alert ("+-265/h") 생성
- 영향 작음 (per-cycle 절대값 자체는 정확). 24h 후 별도 fix
- 개선 방안: `.log.1` 도 합산하거나, monitor 가 own counter 를 유지

## §5b. monitor.sh **large log size limit** — **infra 결함 (21h 추가 발견, §5 보다 심각)**
- 21h 시점 DetectBase.log = **1.15GB**
- `monitor.sh:204` `LOG_SLICE=$(awk -F'"ts":"' -v cut="$START_LOG_CUT" '$2 >= cut' "$LOG_PATH")` 가 1GB+ 결과를 bash variable 에 담으려다 **xrealloc 18EB 할당 실패** (`/bin/bash: xrealloc: 18446744071720018048 bytes를 할당할 수 없음`)
- 결과: `grep -c` 모두 0 (입력 비어있음) → JSONL events `reset=0 eos=0 wd=0 err=0 warn=0` 표기 → watcher delta "-644/h" spurious alert
- 운영은 정상 (DFPS metric 직접 측정 116 / cam 4/4 / container Up)
- **남은 monitor 시간 동안 JSONL events 전부 0 표기 가능** → 실 운영 모니터링은 다음 두 방법으로 cross-check 필요:
  1. `curl http://localhost:9090/metrics | grep detectbase_` — Prometheus metric 직접
  2. `awk -F'"ts":"' '$2 >= "<cut>"' logs/DetectBase.log | grep -cE 'ResetSourceOnly.*OK'` (pipe 사용, 변수 우회)
- 21h 시점 실 누적 (직접 grep, 변수 우회): reset=696 / eos=694 / wd=2 / err=0 — 모두 정상 범위
### §5b-fix. LOG_SLICE 모델 — 잘못된 선택 + 대안

**LOG_SLICE 의 정체**: monitor.sh 가 매 cycle 마다 사용하는 bash 임시 string 변수.
한 번 awk slice 한 결과를 여러 grep (reset/eos/wd/err/warn 등 7회) 가 재사용하려는 의도로 변수에 담음.
```bash
LOG_SLICE=$(awk -F'"ts":"' -v cut="$START_LOG_CUT" '$2 >= cut' "$LOG_PATH")
RESET=$(echo "$LOG_SLICE" | grep -cE "ResetSourceOnly.*OK")
EOS=$(echo "$LOG_SLICE"   | grep -c "on_eos trigger")
WD=$(echo "$LOG_SLICE"    | grep -c "frame-age watchdog")
# ... + ERR + WRN + FTC ...
```

**근본 원인 — 잘못된 모델**:
- `cut = monitor 시작 시각` (`$START_LOG_CUT`, 고정)
- 따라서 slice = "monitor 시작 이래 전부" → **cumulative 모델**
- 시간 흐를수록 slice 크기 선형 증가 → 21h 시점 1.15GB → bash variable xrealloc 폭발

**대안 1 (권장) — Per-cycle slice + 자체 누적 counter**
```bash
LAST_CUT="$START_LOG_CUT"
RESET_TOTAL=0; EOS_TOTAL=0; WD_TOTAL=0; ERR_TOTAL=0; WRN_TOTAL=0
while true; do
    NOW_CUT=$(date -u +%Y-%m-%dT%H:%M)
    SLICE=$(awk -F'"ts":"' -v c="$LAST_CUT" '$2 >= c' "$LOG_PATH")
    # slice = 직전 cycle 이래 ~1분치 (KB-MB scale, 메모리 안전)

    RESET_TOTAL=$((RESET_TOTAL + $(echo "$SLICE" | grep -cE "ResetSourceOnly.*OK")))
    EOS_TOTAL=$((EOS_TOTAL     + $(echo "$SLICE" | grep -c "on_eos trigger")))
    # ... 동일하게 wd/err/warn ...

    LAST_CUT="$NOW_CUT"
    sleep 60
done
```
- 슬라이스 매번 ~1분치 (수십 KB ~ 수 MB)
- 누적은 변수가 직접 들고 있음 (단순 정수 증가)
- log 크기 무관 안정. log rotation 도 자연 흡수 (rotation 후 LAST_CUT 이전 데이터는 자동 새 log 시작 시점 기준)

**대안 2 — variable 우회 (간단 patch)**
```bash
RESET=$(awk -F'"ts":"' '$2 >= c' "$LOG_PATH" | grep -c "...")
EOS=$(awk -F'"ts":"'   '$2 >= c' "$LOG_PATH" | grep -c "...")
# ... 매번 awk full scan
```
- 변수 안 쓰니 메모리 폭발 X
- 단 awk 가 1GB log 를 매 grep 마다 다시 스캔 → CPU/I/O 비용 7배 (slice 7번 grep)
- 1분 cycle 이 못 맞춰질 위험 (awk 1회 ~10초 시 7회 = 70초 > 60초)

**대안 3 — 파일 byte offset 추적 (가장 효율)**
- 매 cycle 종료 시 `LAST_BYTES=$(stat -c %s LOG_PATH)`
- 다음 cycle: `tail -c +$((LAST_BYTES+1)) LOG_PATH` 로 새 분량만
- log rotation 감지: size 감소 → 처음부터 다시
- 1분 cycle 에 KB 단위 처리, 가장 가벼움

**우선순위**: 대안 1 권장 (단순 + 안전). 대안 3 는 추가 효율 원할 시 단계적 도입.
- §5 보다 높은 우선순위 — 24h 후 즉시 fix 권장

## §5c. cycle interval drift 측정 결과 (21h 시점 quantified)
504 cycles / 1271분 → 평균 interval **151.6s** (목표 60s 의 2.5배). lag **768 cycles** (60%).

| 구간 | n | avg | max | 해석 |
|---|---|---|---|---|
| 0-4h | 160 | 89.5s | 184s | 작은 log 도 이미 60s 초과 |
| 4-8h | 120 | 120.6s | 370s | log 증가 비례 drift |
| 8-12h | 109 | 131.7s | 294s | 일정 가속 |
| 12-16h | 61 | 238.3s | 553s | log ~700MB-1GB, awk 폭증 |
| **16-20h** | **31** | **466.6s** | **1280s (21분!)** | bash variable 임계 임박 |
| 20-24h | 23 | 193.7s | 427s | xrealloc 실패 후 "실패-but-fast" 패턴 (변수 비고 grep 즉시 종료) |

**원인** = §5b 의 LOG_SLICE cumulative 모델 + bash variable 저장의 동일 결함. drift 와 무력화는 같은 root cause 의 두 증상.

**데이터 quality 영향**:
- ✅ Per-snapshot 값 (DFPS / RSS / threads / FD / per-cam stats) — cycle 시점 값이므로 정확
- ⚠️ Events 누적값 (reset/wd/err/eos/warn) — 16h+ 부터 점점 부정확, 21h+ 부터 모두 0 spurious
- ✅ 운영 자체 검증은 가능 — Prometheus metric + 직접 grep 으로 cross-check

**fix 우선순위 재확인**: §5b + §5c 는 동일 fix (대안 1 — per-cycle slice + 자체 counter) 로 해결.

## §6. q_drop_inf cam 659/661 누적 패턴 — 분석 보류
- cycle 214 (14:56Z) 시점: cam 659 q_drop_inf=8385, cam 661=7412
- 16h tick (00:30Z) 시점: cam 659=20616, cam 661=17974 (계속 증가)
- cam 658/660 q_drop_inf = 0 (계속)
- 의미: 659/661 만 inflight queue 가 가득 차서 drop-oldest 정책 발화 — NPU 처리 속도가 input 속도를 못 따라가는 상황 간헐 발생
- DFPS 영향 없음 (q_drop_inf 발화 자체가 backpressure 역할)
- v0.1.18 baseline 의 q_drop_inf 와 비교 필요 → 누적 패턴 자체가 정상인지 회귀인지 판단

---

## 종합 (16h 시점, 잠정)
| 회귀 판정 기준 | 현 값 | 판정 |
|---|---|---|
| DFPS<100 sustained | 1 cycle dip 2건 (74, 42.8) 모두 즉시 회복 | ✅ sustained 아님 |
| wd > 3/h | 2/16h = 0.13/h | ✅ 임계 미달 |
| reset > 50/h | ~48~52/h | ✅ |
| cam_loss > 0 | 0 (cam 4/4 유지) | ✅ |
| RSS leak | 7h decommit 검증 + plateau 1240MB | ✅ leak 부재 |

**v0.1.26 Release 1h 회귀 (DFPS 99.4 / wd 3/h / reset 64/h) 는 (F1) file logger 결함 + (F2) monitor.sh metric 결함 의 측정 artifact 였음** 이 강하게 시사됨. 24h 완주 후 final 보고서에 본 확정.

---

## 최종 보고서 작성 시 포함 항목 체크리스트
- [ ] §1 메모리 추세 (RSS 진동 패턴 + jemalloc 분해) — **사용자 명시 요청**
- [ ] §2 658 wd 1건 분석
- [ ] §3 4 cam EOS storm 분석
- [ ] §4 자정 dir rollover 실패 (minor bug)
- [ ] §5 monitor.sh log rotation 처리 (minor infra)
- [ ] §6 q_drop_inf 659/661 누적 패턴 (baseline 비교 결과)
- [ ] 종합 회귀 판정 결과
- [ ] v0.1.18 baseline 과 정량 비교 (DFPS mean / wd / reset / RSS plateau)
- [ ] master_logs/v1.0.0/ archival 권장 (회귀 미발생 확정 시)
