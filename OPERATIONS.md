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
./detectbase.sh audit                    # 전체 (light default: ASan 60m + TSan 60m, ~1h 30min, develop/내부 검증)
./detectbase.sh audit --strict           # master merge gate (ASan 240m + TSan 60m, ~5h)
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

**파일**: `logs/DetectBase.log` (logrotate **daily + rotate 14** — 매일 회전, 14일치 보관, 압축)
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
| `detectbase_dfps_total` | 카메라 수 × ~29 (예: 4 cam → ~115. NPU multi-core PR #6 이후 baseline. single-core 시절엔 ×13 = 52~56 이었음) | < 절반 (~57) | 0 (NPU/RTSP 장애) |
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
| `Engine ... timeout (Waited max 5000 ms)` | NPU 추론 5초 timeout (1회는 일시 — 5초 초과 시 ERROR) | 일시 발생은 무시. 지속 시 NPU 디바이스 + RKNN 모델 호환성 점검 |
| `SocketIO Connection Faild, Timeout(10s)` | SocketIO 단절 (10s connect timeout) | reconnect 자동 (지수 백오프, max 30s). socketio_reconnect_total 증가 추이 확인 |
| `SocketIOEventBind() Failed` | SocketIO event handler 등록 실패 (init 단계) | SocketIO listener 설정 점검. 단발이면 재시도, 지속 시 broker 호환성 |
| `RtspHandler->RunRTSP() failed` / `InitializeRTSPWithStaticCameraList FAILED` | RTSP unit start 실패 (init 단계) | 카메라 IP / 인증 / NetworkSettings.json 점검. RTSP unit 자동 재연결 (운영 중 단발) |
| `GRPC Client[<name>] init exception: <msg>` | GRPC peer 연결 실패 (init 단계 throw 발생) | peer 주소 점검. NetworkSettings.json `GRPC_Peers` 확인. exception 메시지로 cause 식별 |
| `[GRPC FAIL][Response/CounterSnapshot] trace_id=...` | GRPC server 응답 실패 | peer 호스트 점검. NetworkSettings.json `GRPC_Server_*` 확인 |
| `RenewAfterReset: skip UnitID ... (Update failed)` | 설정 부분 실패 (graceful degradation) | setting_partial_failure_total 메트릭 확인. 실패 unit 의 설정 형식 점검 |

**비고**: `imwrite ... failed` 같은 ERROR 메시지는 현 코드에 없음 (디스크 가득 시 WARN `imwrite skipped` 로 처리, ERROR 단계 아님). 디스크 권한 issue 는 emergency_cleanup 작동 안 함 + L1 차단 외 별도 ERROR 출력은 없음.

### WARN

| 메시지 패턴 | 의미 | 대응 |
|---|---|---|
| `GstRtspClient[N] frame-age watchdog: <s>s 무프레임 → 강제 reset` | RTSP stream 일시 단절 — 12s 무프레임 시 force reset 트리거 (v0.1.18 부터 TeardownPipeline unref-skip 적용) | 재연결 자동. 빈발 시 cam server / 네트워크 점검 |
| `GstRtspReceiver[N] EOS received src=...` | RTSP stream EOS (5분 mp4 cycle 또는 server 명시 종료) | 자동 재연결. 정상 동작 |
| `GstRtspReceiver[N] GstRTSPSrcTimeout cause=<n>` | RTSP/RTCP 통신 timeout (RTCP / TEARDOWN 등) | RTCP cause 는 measure 만 (reconnect 안 함). 다른 cause 는 자동 reconnect. 빈발 시 server 점검 |
| `GstRtspReceiver[N] bus WARNING src=... debug=...` | GStreamer pipeline 내부 warning (rtpmanager / udpsrc 등) | 대부분 무해. 동일 패턴 빈발 시 GST_DEBUG=2 진단 |
| `TeardownPipeline[N] — pipeline NULL transition 실패 (result=%d, state=%d). gst_object_unref 건너뜀` | v0.1.18 TeardownPipeline unref-skip 발화 (5s timeout 시 의도된 leak, OS cleanup 의존) | **v0.1.18 fix path 발화 시그널**. 빈발 시 → stuck 재발 → escalation Step 2-4 (자세한 내용 NEXT_SESSION) |
| (운영 측 별도 출력 없음 — 내부 정상) | v0.1.26 부터 각 GstRtspReceiver/ProxyServer 가 dedicated `GMainContext` 보유 + bus watch/jitter timer 가 `GSource*` 멤버 보관 + `g_source_destroy()` 로 정리 (UAF fix). multi-cam coupling 해소 + TSan SEGV 회귀 0. | 운영 시 추가 대응 불필요. audit 재실행 시 baseline 비교만. |
| `imwrite skipped — frame disk ... ≥ 90%` | L1 사전 차단 작동 (디스크 90%) | 정상 동작. emergency cleanup 작동 확인. 만성이면 디스크 증설 |
| `EMERGENCY cleanup` | L2-Emergency 작동 (80% 도달) | 정상 동작. 주기 빈도 높으면 retention 일수 줄이거나 디스크 증설 |
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

원인: thread join 무한 대기 (RTSP / GRPC / NPU 어딘가 hang). v0.1.18 부터 `GstRtspReceiver::TeardownPipeline` 의 `gst_object_unref(pipeline_)` 가 5s timeout 시 건너뛰는 unref-skip 적용되어 cam_loss escalate 발생 시 process 자체는 재시작 가능 (의도된 leak, OS cleanup 의존). v0.1.26 부터 GMainContext per-instance 격리 + UAF fix 로 timer/bus_watch lifecycle 결함 결정적 제거됨 (TSan SEGV 0).

대응:
1. `docker logs --tail 50 detectbase_service` — 마지막 로그 확인 (`#0X. Stop ...` 어디서 멈췄는지)
2. SIGKILL 후 재시작 (`./detectbase.sh restart`)
3. 빈발 시 `./detectbase.sh audit --only tsan` (light = TSan 1h, race/deadlock 검사) 또는 `--strict` (master merge gate)

### §5.3 카메라 연결 안 됨 (1대 또는 전부)

```
{"lvl":"WARN","msg":"GstRtspClient[XXX] frame-age watchdog: <s>s 무프레임 → 강제 reset"}
{"lvl":"WARN","msg":"GstRtspReceiver[XXX] GstRTSPSrcTimeout cause=<n>"}
```

체크:
1. ping 으로 카메라 IP 도달성
2. `curl <rtsp_url>` 접근 시도
3. NetworkSettings.json 의 카메라 RTSP URL 정확성
4. 카메라 측 RTSP 서버 / 인증 토큰

### §5.4 NPU 추론 timeout 빈발

```
{"lvl":"WARN","msg":"CAM[XXX] Engine YoloV5s_Airockchip_RKNN timeout (Waited max 5000 ms)"}
```
(engine name 은 EngineSettings.json 의 TagName 그대로 출력됨)

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
# 강도 모드 (2026-05-28 도입)
./detectbase.sh audit                    # light default — ASan 60m + TSan 60m (~1h 30min, develop/내부 검증)
./detectbase.sh audit --light            # 명시적 light (default 와 동일)
./detectbase.sh audit --strict           # ASan 240m + TSan 60m (~5h, master merge gate)

# 묶음 모드
./detectbase.sh audit --no-tsan          # TSan 제외 (정적 + ASan/UBSan)
./detectbase.sh audit --with-tsan        # = 전체 (backward compat)

# 단독 모드 — 변경 검증 시 필요 도구만 빠르게
./detectbase.sh audit --only cppcheck    # 정적 (~1분)
./detectbase.sh audit --only clang-tidy  # 정적 (~10분)
./detectbase.sh audit --only asan|ubsan  # 동적 (운영 정지, light 1h / strict 4h)
./detectbase.sh audit --only tsan        # 동적 (운영 정지, default 1h)
```

환경변수 override (강도 모드보다 우선):
- `ASAN_DURATION_MIN` (light default **60분**, strict default **240분**, **최소 60분 강제**): `ASAN_DURATION_MIN=120 ./detectbase.sh audit --only asan`
- `TSAN_DURATION_SEC` (default **3600초 = 1시간**, **최소 3600초 강제** — 3600 미만이면 3600으로 자동 보정): `TSAN_DURATION_SEC=3600 ./detectbase.sh audit --only tsan`

**강도 모드 선택 기준 (CLAUDE.md master merge gate 와 동기)**:
- **light** (default) — develop 머지 / 내부 검증 / branch 자체 검증. 자체 코드 결함 회귀 탐지 목적. ~1h 30min.
- **strict** — master merge gate. patch/minor bump 시 audit 5종 통과 요건의 audit 가 이 강도. ~5h. `master_logs/v<버전>/` baseline 도 strict 결과.

**결과**: `logs/audit_<timestamp>/`
- `summary.txt` — 카운트 요약
- `cppcheck.log` / `clangtidy.log` / `asan_run.log` / `tsan_run.log` (raw)

**기준 (DetectBase production-ready baseline)** — strict 강도, 산출물: `master_logs/v0.1.18/audit_20260527_091456/` (2026-05-27 v0.1.18 release):
- cppcheck 자체 코드 결함: **59건** (false positive 18 + Profiler 자연정리 9 + cppcheck syntax quirk suppress 비효력 보존)
- clang-tidy warning: **0건 ✅** (PR #9 NOLINT 24 + PR #13 진짜 fix 후 30 → 0 도달, 5/27 까지 동일 유지)
- ASan/UBSan: 자체 코드 leak **0 ✅**. 외부 lib leak 만 수용 (librknnrt rknn_init startup + GStreamer rtpmanager runtime ~340 MB/year). 4h strict run 기준 1.24 MB / 11,515 alloc.
- TSan: **자체 코드 진짜 race 0 ✅** (4 root cause 모두 fix: SioHandler UAF / InferenceCounter map / RegisterMetricsOnce / SafeQueue shared_ptr ref). 1h strict run 기준 WARNING **172** (SIGKILL false positive + 외부 lib + 추적 한계).

**v0.1.26 light audit (5/28) 검증 결과 — UAF fix 적용 후 회귀 0**:
- ASan/UBSan 1h: 1.22 MB / 10,639 alloc (baseline 4h strict 와 동등 — startup+cycle dominated, 시간 비례 X). 자체 leak 0.
- TSan 1h: WARNING **158** (baseline 172, -8%), **SEGV 0 ✅** — `g_source_remove` → `g_source_destroy` UAF fix 결정적 검증 (직전 audit 의 `OnJitterStatsTimer` SEGV 완전 제거).

**Sanitizer 환경의 throughput artifact — 무시할 WARN**:
- `Engine Input Queue Full (128/128). Dropping request to prevent lag.` — ASan instrument 2-3x slowdown (TSan 100x) 으로 NPU pipeline throughput < cam supply rate 발생 시 backpressure (EngineLoadBalancer.cpp:218) 가 의도된 대로 drop. **정상 운영에선 거의 0회, sanitizer 빌드에선 frequent (예: 5/24 ASan run 27.9% 비율)**. audit 결론은 LeakSanitizer / TSan WARNING summary 만 평가하면 되고 Queue Full 빈도는 의미 없음.
- TSan 의 경우 `__SANITIZE_THREAD__` 컴파일 guard 로 `inference_per_cams_fps_limit=1` 강제 patch 있음 (README §15) — packet drop 회피용. ASan 은 별도 패치 없이 backpressure 자연 throttle.
- 같은 이유로 sanitizer 빌드 시 `DFPS` 가 정상 운영보다 낮음 — 비교 기준 아님.

자세한 내역: [.DOCS/AUDIT_REPORT_20260519.md](.DOCS/AUDIT_REPORT_20260519.md) (v0.1.0 baseline, historical), [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md), [.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md](.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md) (v1.0.0 진입 전 race 점검, 자체 race 0건 확정). 최신 production-ready baseline = `master_logs/v0.1.18/audit_20260527_091456/` (5/27 v0.1.18 release, strict 강도). master tag 별 audit 산출물은 `master_logs/v<버전>/audit_*/` 로 archival (5/27 절차 정착).

이 수치가 크게 늘면 변경된 코드의 결함 의심.

---

## §7. 백업 / 복구

### logrotate (자동)

`/etc/logrotate.d/detectbase` 또는 `scripts/logrotate.detectbase` 참고. **daily + rotate 14** (매일 회전, 14일치 보관, 압축, copytruncate).

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

### master_logs 검증 증빙 (release archival, 5/27 절차 정착)

master 머지 시 검증 증빙은 `master_logs/v<버전>/` 에 보관 (git tracked):
- `audit_<stamp>/` 전체 (cppcheck / clang-tidy / asan_run / tsan_run / summary, strict 강도)
- 머지 윈도우의 monitor JSONL
- 머지 근거 요약 `README.md`

`logs/audit_*/` 가 gitignored 라 develop 머지 직전 별도 chore branch 로 master_logs/ 안 이동 + PR → develop, 그 후 develop → master `--no-ff` merge. 절차 자세히는 `.claude/skills/git-workflow.md` §master_logs Archival.

---

## §8. Alert escalation (참고 옵션, 미적용)

현재 `monitor.sh` 의 alert mark (`★storm` / `★err` / `★dfps_low` / `★memory` / `★watchdog` / `★ftc` / `★cam_loss`) 는 **JSONL + stdout 만**. 외부 알림 채널 송신 X. 운영자가 직접 monitor 를 보거나 JSONL 을 tail 해야 인지함.

본 프로젝트 현 운영 환경 (single-server / single-user / 보안 미중요 사이트) 에선 미적용. 운영팀 분산 / 24h 즉시 알림 필요 시 다음 두 가지 옵션 가능:

**옵션 A — monitor.sh 안에 외부 알림 채널 직접 통합** (간단):
- Slack webhook: `curl -X POST -d '{"text":"..."}' <webhook_url>`
- Email: `sendmail` / `mailx`
- Telegram bot: `curl https://api.telegram.org/bot<token>/sendMessage`
- 일반 webhook: `curl -X POST <ops-endpoint>`
- alert mark detection 시 위 명령 호출 한 줄 추가

**옵션 B — Prometheus alertmanager 활용** (확장성, §8 권장 yaml 참조):
- 별도 Prometheus + alertmanager 설치 필요
- `detectbase_*` 메트릭 기반 rule 작성 (예: 아래 §8 권장 yaml)
- 외부 알림 채널 라우팅은 alertmanager 설정

본 환경 비적용. 필요해지면 추가 작업.

---

## §8.1 Prometheus alert rules 권장

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
- [ ] DFPS 정상 (카메라 수 × ~29 이상, 4cam → ≥115 권장)
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
