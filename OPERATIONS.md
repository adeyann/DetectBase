# DetectBase 운영 가이드 (OPERATIONS.md)

> 운영자용 트러블슈팅 / 알림 / 백업 가이드.
> 사용자용 빌드/실행 기본은 [README.md](README.md), 코드 구조는 [code/README.md](code/README.md), 현재 작업 계획은 [logs/NEXT_SESSION.md](logs/NEXT_SESSION.md), 3차 리뷰 baseline 은 [.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md](.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md).

---

## §1. 빌드 / 시작 / 종료 cheat sheet

```bash
./detectbase.sh build      # Docker 이미지 빌드 + init (proto 재생성)
./detectbase.sh compile    # C++ 소스 컴파일 (~2분)
./detectbase.sh start      # 서비스 시작 + 로그 follow
./detectbase.sh stop       # graceful shutdown (~10초, PROGRAM QUIT SUCCESS 까지)
./detectbase.sh restart    # 정지 후 시작
./detectbase.sh logs       # 로그 follow only
./detectbase.sh audit                    # 전체 (cppcheck + clang-tidy + ASan/UBSan + TSan)
./detectbase.sh audit --no-tsan          # TSan 제외
./detectbase.sh audit --only <tool>      # 단독 (cppcheck|clang-tidy|asan|ubsan|tsan)
./detectbase.sh prune      # 안 쓰는 도커 리소스 정리
./detectbase.sh all        # build → init → compile → start (전체)
```

**graceful shutdown 순서** (정상):
```
###. PROGRAM QUIT START
#00 Stop GRPC Server          (외부 client 의 새 요청 수신 차단)
#01 Terminate Engines         (NPU 추론 종료)
#02 Terminate Load Balancer   (engine_input_q dispatcher 종료)
#03 Stop Service Implements   (Detector Block / 모든 Unit thread join, 5초 정도)
#04 Stop Network Flow         (SocketIO/REST/GRPC client)
#05 Stop IO Stream Manager
###. PROGRAM QUIT SUCCESS     (이 줄 출력 후 docker container 종료)
```

PROGRAM QUIT SUCCESS 안 나오면 비정상 — §5 트러블슈팅 §5.1 참고.

---

## §2. 로그 / 메트릭 모니터링

### 로그 (이벤트 기록)

**파일**: `logs/DetectBase.log` (logrotate 100MiB × 7개)
**포맷**: JSON 한 줄 (구조화)

```bash
# 실시간 follow
tail -f logs/DetectBase.log

# ERROR 만
grep '"lvl":"ERROR"' logs/DetectBase.log | tail -30

# 카메라별 추적 (correlation_id)
grep '"correlation_id":"sys-detector-658"' logs/DetectBase.log

# IOWorker 추적
grep '"correlation_id":"sys-io_worker-' logs/DetectBase.log

# SocketIO 인바운드 추적
grep '"correlation_id":"evt-' logs/DetectBase.log
```

### 메트릭 (운영 지표 — 시계열)

**endpoint**: `http://<host>:9090/metrics` (Prometheus 표준)
**갱신**: 외부 서버가 polling. 권장 scrape interval = 5~15초

```bash
# 전체 dump
curl -s http://localhost:9090/metrics | grep "^detectbase_"

# DFPS / 카메라 수
curl -s http://localhost:9090/metrics | grep -E "^detectbase_(dfps|camera_count)"

# 에러 누적 (운영 가시성)
curl -s http://localhost:9090/metrics | grep "^detectbase_errors_total"

# drop 메트릭 (백프레셔 인지)
curl -s http://localhost:9090/metrics | grep -E 'errors_total\{type="(emit_drop|io_work_drop|engine_input_q_drop|logger_fail|setting_callback)"\}'

# 디스크 방어
curl -s http://localhost:9090/metrics | grep -E "^detectbase_frame_(disk|emergency|cleanup)"

# 설정 부분 실패
curl -s http://localhost:9090/metrics | grep "setting_partial_failure_total"

# GRPC (활성 시)
curl -s http://localhost:9090/metrics | grep -E "^detectbase_grpc_"
```

### 의미 있는 임계점

| 메트릭 | 정상 | 주의 | 위험 |
|---|---|---|---|
| `detectbase_dfps_total` | 카메라 수 × 13~14 (예: 4 cam → 52~56) | < 절반 | 0 (NPU/RTSP 장애) |
| `detectbase_camera_count{state="active"}` | = registered | < registered | 0 |
| `detectbase_errors_total{type="imwrite_fail"}` | 0/min | > 0/min 지속 | > 10/min |
| `detectbase_errors_total{type="emit_drop"}` | 0/min | > 0/min (SocketIO 누적) | 빠르게 증가 |
| `detectbase_errors_total{type="io_work_drop"}` | 0/min | > 0/min (디스크 IO 누적) | 빠르게 증가 |
| `detectbase_errors_total{type="logger_fail"}` | 0 | 1~5 (외부 IO 일시 실패) | > 10 |
| `detectbase_errors_total{type="setting_callback"}` | 0 | > 0 (설정 callback 실패) | 빠르게 증가 |
| `detectbase_frame_disk_used_pct` | < 80% | 80~90% | ≥ 90% (L1 차단) |
| `detectbase_frame_emergency_cleanup_total{type="day_dir"}` | 안 증가 | 1~5/일 | 지속 증가 (디스크 부족) |
| `detectbase_frame_emergency_cleanup_total{type="half_files"}` | 안 증가 | 1~10/일 | 지속 증가 (당일 누적 과다) |
| `detectbase_socketio_reconnect_total` | 0 | 1~5/일 (망 일시 단절) | 지속 증가 |

---

## §3. ERROR / WARN 의미와 대응

### ERROR

| 메시지 패턴 | 의미 | 대응 |
|---|---|---|
| `Engine ... timeout` | NPU 추론 5초 timeout (1회는 일시 — 5초 초과 시 ERROR) | 일시 발생은 무시. 지속 시 NPU 디바이스 + RKNN 모델 호환성 점검 |
| `imwrite ... failed` | 디스크 쓰기 실패 (권한 / 디스크 가득 / IO 에러) | df / 디렉토리 권한 확인. emergency_cleanup 작동 여부 확인 |
| `socketio connection lost` | SocketIO 단절 | reconnect 자동 (지수 백오프). reconnect_total 증가 추이 확인 |
| `RTSP ... failed` | RTSP 카메라 연결 실패 | 카메라 IP / 인증 / 네트워크 점검. RTSP unit 자동 재연결 |
| `GRPC ... init failed` | GRPC peer 연결 실패 | peer 주소 점검. NetworkSettings.json grpc_peers 확인 |
| `RenewAfterReset: skip UnitID ... (Update failed)` | 설정 부분 실패 (graceful degradation) | setting_partial_failure_total 메트릭 확인. 실패 unit 의 설정 형식 점검 |

### WARN

| 메시지 패턴 | 의미 | 대응 |
|---|---|---|
| `decoded frame not received...` | RTSP stream 일시 단절 (1분 timeout) | 재연결 진행 중. 5분 이상 지속 시 카메라 점검 |
| `imwrite skipped — frame disk ... ≥ 90%` | L1 사전 차단 작동 (디스크 90%) | 정상 동작. emergency cleanup 작동 확인. 만성이면 디스크 증설 |
| `EMERGENCY cleanup` | L2-Emergency 작동 (80% 도달) | 정상 동작. 주기 빈도 높으면 retention 일수 줄이거나 디스크 증설 |
| `Packet queue full ... Dropping non-key frame` | RTSP packet drop (대부분 startup 또는 instrumentation 빌드) | 정상 운영에선 거의 발생 X. TSan 빌드 시 정상 |
| `Engine Input Queue Full (128/128). Dropping request to prevent lag.` | NPU 입력 큐 backpressure (EngineLoadBalancer.cpp:218) — drop new request | **정상 운영에선 거의 발생 X**. ASan/TSan 등 sanitizer 빌드 시 frequent (instrument 2-3x slowdown 으로 NPU pipeline throughput < cam supply → queue 누적 → drop). audit run 의 결론은 LeakSanitizer/TSan WARNING 만 보면 됨, Queue Full 빈도는 무시 |
| `Engine ... timeout (Waited max 5000 ms)` | NPU 추론 5초 timeout | 한두 번은 자연. 빈발 시 NPU 점검 |
| `GRPC peer ...` | GRPC peer 연결 일시 끊김 | 자동 재연결. 잦으면 peer 호스트 점검 |

---

## §4. 디스크 방어 정책 (P54)

```
/frame 디스크 사용률 (frame_disk_used_pct) → 자동 청소 작동:

[정상 운영]
└─ L2-Regular 정기 청소 (1시간 주기)
    └─ 7일 이전 일자 폴더 삭제
    └─ frame_cleanup_deleted_total{} 증가

[80% 도달]
└─ L2-Emergency 비상 청소 (5분 cool-down)
    └─ 과거 일자 폴더 우선 삭제
    └─ 당일만 남으면 절반 (half_files) 삭제
    └─ frame_emergency_cleanup_total{type="day_dir"|"half_files"} 증가

[90% 도달]
└─ L1 사전 차단
    └─ imwrite skip (저장 안 함, 분석은 계속)
    └─ imwrite_skipped_total{reason="disk_full"} 증가
```

**검증**:
```bash
# 디스크 사용률
docker exec detectbase_service df -h /frame

# 일자별 폴더 (오래된 순)
docker exec detectbase_service bash -c 'cd /frame && ls -td */ | tail -10'

# emergency_cleanup 누적 추이
curl -s http://localhost:9090/metrics | grep frame_emergency_cleanup_total
```

**디스크 부족 만성 시**:
- 옵션 1: 디스크 증설 (호스트 마운트 변경)
- 옵션 2: `FRAME_RETENTION_DAYS` 7 → 3 으로 줄이기 ([RtspDetectorUnit.cpp](code/Main/DETECTOR/src/RtspDetectorUnit.cpp))
- 옵션 3: imwrite 자체 비활성화 (큰 작업)

---

## §5. 트러블슈팅

### §5.1 서비스 시작 안 됨

```
{"lvl":"ERROR","msg":"Failed to ..."}
컨테이너 즉시 종료
```

체크 순서:
1. `docker logs detectbase_service` — 정확한 ERROR 메시지
2. `/usr/lib/librknnrt.so` 호스트에 존재 확인 (`ls -la /usr/lib/librknnrt.so`)
3. `/dev/dri/renderD129` NPU 디바이스 존재 확인 (`ls -la /dev/dri/`)
4. `settings/NetworkSettings.json` / `EngineSettings.json` 형식 검증 (`python3 -m json.tool < settings/NetworkSettings.json`)
5. `engines/*.rknn` 파일 존재 확인

NPU 디바이스 없을 때:
```bash
sudo insmod /lib/modules/.../rknpu.ko
ls /dev/dri/renderD129  # 생성 확인
```

### §5.2 graceful shutdown 안 됨

```
docker stop detectbase_service
# 30초 후에도 안 종료됨 → SIGKILL 강제
```

원인: thread join 무한 대기 (RTSP / GRPC / NPU 어딘가 hang)

대응:
1. `docker logs --tail 50 detectbase_service` — 마지막 로그 확인 (`#0X. Stop ...` 어디서 멈췄는지)
2. SIGKILL 후 재시작 (`./detectbase.sh restart`)
3. 빈발 시 `./detectbase.sh audit --only tsan` 으로 race / deadlock 검사

### §5.3 카메라 연결 안 됨 (1대 또는 전부)

```
{"lvl":"WARN","msg":"CAM[XXX] decoded frame not received..."}
```

체크:
1. ping 으로 카메라 IP 도달성
2. `curl <rtsp_url>` 접근 시도
3. NetworkSettings.json 의 카메라 RTSP URL 정확성
4. 카메라 측 RTSP 서버 / 인증 토큰

### §5.4 NPU 추론 timeout 빈발

```
{"lvl":"WARN","msg":"CAM[XXX] Engine DetectionEngine timeout"}
```

체크:
1. `dmesg | grep rknpu` — 커널 로그에서 NPU 에러
2. `cat /sys/kernel/debug/rknpu/load` (있다면) — NPU 부하
3. RKNN 모델 / runtime 버전 호환성 (`librknnrt.so` 1.5.2 / 모델 1.5.2)

### §5.5 메트릭 endpoint 연결 안 됨

```
curl http://localhost:9090/metrics
# Connection refused
```

원인: prometheus exposer 시작 실패 (포트 충돌 또는 init 단계 실패)

체크:
1. `ss -tlnp | grep 9090` — 포트 점유 확인
2. logs 의 `MetricsRegistry: HTTP exposer ...` 라인 (정상 시 9090 listen 성공)

### §5.6 디스크 방어 안 됨 (90% 넘어 가득)

L1 사전 차단 작동 → imwrite_skipped_total 증가하면서도 운영 계속.
운영 계속하지만 frame 저장 안 되니까:
- 임시 (즉시): emergency cleanup 강제 트리거 — `docker exec detectbase_service bash -c 'find /frame -type d -mtime +0 | sort | head -5'` 로 오래된 폴더 확인 후 수동 삭제
- 영구: §4 옵션 1/2/3

---

## §6. audit 사용법 (운영 검증)

분기 프로젝트 fork 또는 큰 변경 후 실행 권고:

```bash
# 묶음 모드
./detectbase.sh audit                    # 전체 (cppcheck + clang-tidy + ASan/UBSan + TSan)
./detectbase.sh audit --no-tsan          # TSan 제외 (정적 + ASan/UBSan)
./detectbase.sh audit --with-tsan        # = 전체 (backward compat)

# 단독 모드 — 변경 검증 시 필요 도구만 빠르게
./detectbase.sh audit --only cppcheck    # 정적 (~1분)
./detectbase.sh audit --only clang-tidy  # 정적 (~10분)
./detectbase.sh audit --only asan|ubsan  # 동적 (운영 정지, default 4h run)
./detectbase.sh audit --only tsan        # 동적 (운영 정지, default 1h run)
```

환경변수 override:
- `ASAN_DURATION_MIN` (default **240분 = 4시간**, **최소 60분 강제** — 60 미만이면 60으로 자동 보정): `ASAN_DURATION_MIN=60 ./detectbase.sh audit --only asan`
- `TSAN_DURATION_SEC` (default **3600초 = 1시간**, **최소 3600초 강제** — 3600 미만이면 3600으로 자동 보정): `TSAN_DURATION_SEC=3600 ./detectbase.sh audit --only tsan`

**결과**: `logs/audit_<timestamp>/`
- `summary.txt` — 카운트 요약
- `cppcheck.log` / `clangtidy.log` / `asan_run.log` / `tsan_run.log` (raw)

**기준 (DetectBase production-ready baseline — 2026-05-20 develop, PR #9 audit cleanup + PR #13 H fix 후)**:
- cppcheck 자체 코드 결함: **59건** (false positive 18 + Profiler 자연정리 9 + cppcheck syntax quirk suppress 비효력 보존)
- clang-tidy warning: **0건 ✅** (PR #9 NOLINT 24 + PR #13 진짜 fix 후 30 → 0 도달)
- ASan/UBSan: startup leak 0 ✅, runtime leak 1 (GStreamer rtpmanager 외부 lib, 수용. 5분 run ~1.2 MB)
- TSan: **자체 코드 진짜 race 0 ✅** (4 root cause 모두 fix). 잔여 137 (SIGKILL false positive + 외부 lib + 추적 한계)

**Sanitizer 환경의 throughput artifact — 무시할 WARN**:
- `Engine Input Queue Full (128/128). Dropping request to prevent lag.` — ASan instrument 2-3x slowdown (TSan 100x) 으로 NPU pipeline throughput < cam supply rate 발생 시 backpressure (EngineLoadBalancer.cpp:218) 가 의도된 대로 drop. **정상 운영에선 거의 0회, sanitizer 빌드에선 frequent (예: 5/24 ASan run 27.9% 비율)**. audit 결론은 LeakSanitizer / TSan WARNING summary 만 평가하면 되고 Queue Full 빈도는 의미 없음.
- TSan 의 경우 `__SANITIZE_THREAD__` 컴파일 guard 로 `inference_per_cams_fps_limit=1` 강제 patch 있음 (README §15) — packet drop 회피용. ASan 은 별도 패치 없이 backpressure 자연 throttle.
- 같은 이유로 sanitizer 빌드 시 `DFPS` 가 정상 운영보다 낮음 — 비교 기준 아님.

자세한 내역: [.DOCS/AUDIT_REPORT_20260519.md](.DOCS/AUDIT_REPORT_20260519.md) (v0.1.0 baseline, historical), [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md). 최신 완료 audit = `logs/audit_20260524_115656/` (5/24 develop, cmake 0.1.10 시점). master tag 별 audit 산출물은 `master_logs/v<버전>/audit_*/` 로 archival (5/27 절차 정착).

이 수치가 크게 늘면 변경된 코드의 결함 의심.

---

## §7. 백업 / 복구

### logrotate (자동)

`/etc/logrotate.d/detectbase` 또는 `scripts/logrotate.detectbase` 참고. 100MiB × 7개 보관.

### frame 디렉토리

- 자동 정리: §4 정책으로 운영 중 자동
- 백업 필요 시 (이벤트 영상): `/frame/YYYY-MM-DD/` 폴더 단위로 외부 저장소 복사 권고

### 설정 파일 백업

```bash
# 변경 전 백업
cp settings/NetworkSettings.json settings/NetworkSettings.json.$(date +%Y%m%d).bak
cp settings/EngineSettings.json settings/EngineSettings.json.$(date +%Y%m%d).bak

# 복구
cp settings/NetworkSettings.json.20260509.bak settings/NetworkSettings.json
./detectbase.sh restart
```

### 컨테이너 이미지

```bash
# 현재 이미지 백업
docker save detectbase:1.0 | gzip > detectbase_1.0_$(date +%Y%m%d).tar.gz

# 복구
docker load < detectbase_1.0_20260509.tar.gz
```

---

## §8. Prometheus alert rules 권장

```yaml
# alerts.yml (Prometheus alertmanager 용 예시)
groups:
- name: detectbase
  rules:
  - alert: DetectBaseDFPSDrop
    expr: detectbase_dfps_total < 20
    for: 5m
    annotations:
      summary: "DFPS 가 절반 이하로 떨어짐 (NPU/RTSP 장애 의심)"

  - alert: DetectBaseCameraInactive
    expr: detectbase_camera_count{state="active"} < detectbase_camera_count{state="registered"}
    for: 10m
    annotations:
      summary: "등록 카메라 중 일부 inactive — RTSP 단절 또는 unit 장애"

  - alert: DetectBaseDiskCritical
    expr: detectbase_frame_disk_used_pct >= 90
    for: 5m
    annotations:
      summary: "디스크 90% 도달 — L1 차단 작동 중. 정리 또는 증설 필요"

  - alert: DetectBaseEmergencyCleanupBurst
    expr: rate(detectbase_frame_emergency_cleanup_total[1h]) > 0.05
    annotations:
      summary: "Emergency cleanup 시간당 빈번 (디스크 부족 만성)"

  - alert: DetectBaseDropMetricBurst
    expr: |
      rate(detectbase_errors_total{type=~"emit_drop|io_work_drop|engine_input_q_drop"}[5m]) > 0.1
    annotations:
      summary: "Drop 메트릭 빠른 증가 (백프레셔 누적)"

  - alert: DetectBaseLoggerFail
    expr: rate(detectbase_errors_total{type="logger_fail"}[5m]) > 0
    annotations:
      summary: "Logger 자체 실패 — 외부 IO 또는 메트릭 시스템 장애"

  - alert: DetectBaseSettingPartialFailure
    expr: increase(detectbase_setting_partial_failure_total[1h]) > 0
    annotations:
      summary: "설정 reset 부분 실패 — 1개 이상 unit 누락 (graceful degradation)"

  - alert: DetectBaseSocketIOReconnectBurst
    expr: rate(detectbase_socketio_reconnect_total[5m]) > 0.05
    annotations:
      summary: "SocketIO 재연결 빈번 — broker 또는 망 단절"
```

---

## §9. 운영 체크리스트

### 일간
- [ ] DFPS 정상 (카메라 수 × 13 이상)
- [ ] 4 cam active (registered 와 일치)
- [ ] ERROR 0
- [ ] frame disk < 80%
- [ ] socketio_reconnect_total 증가 추이 점검

### 주간
- [ ] emergency_cleanup_total 증가 추이 (정상 < 5/주)
- [ ] frame_cleanup_deleted_total 정기 청소 작동 확인
- [ ] log 파일 크기 (logrotate 정상)
- [ ] 디스크 사용량 추이 (cap 잘 됐는지)

### 월간
- [ ] 컨테이너 이미지 백업
- [ ] 설정 파일 백업
- [ ] OS 보안 업데이트 (호스트 측)
- [ ] NPU 드라이버 업데이트 검토

### 분기 프로젝트 fork 시 / 큰 변경 시
- [ ] `./detectbase.sh audit` 실행 → 결과 비교 (`logs/audit_<timestamp>/summary.txt`)
- [ ] graceful shutdown 시퀀스 검증 (`./detectbase.sh stop` → PROGRAM QUIT SUCCESS)
- [ ] 운영 60분 누적 (RSS / FD / Threads leak 없음 확인)
- [ ] 메트릭 endpoint 부하 (`for i in {1..100}; do curl -s ... & done`)
