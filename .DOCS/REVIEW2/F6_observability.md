# F6 — Observability (Logger + Metrics + Disk Defense)

## §1. Why

운영 시 시스템 상태를 외부에서 관측할 수 있게 하는 흐름.

3 영역:
1. **Logger** — 모든 흐름의 동작 기록. JSON 한 줄 + correlation_id 로 cross-thread 추적
2. **Metrics** — Prometheus exporter (port 9090). pull 방식 수집. 18개 메트릭 등록
3. **Disk Defense** — `/frame` 디스크 3-Layer 방어 (L1 사전 차단 / L2-Regular 정기 청소 / L2-Emergency 비상 청소 / L3 메트릭). 운영 중 디스크 100% 로 인한 컨테이너 mkdir 실패 사례 후 도입.

제거 시 잃는 것: 외부 관측 불가. 디스크 만수 시 자가 회복 불가 → 컨테이너 동작 중단 가능.

---

## §2. Roster

### Primary (F6)

| 카테고리 | 파일 |
|---|---|
| **Logger** | [logger/MgenLogger.{h,cpp}](../../code/BasicLibs/core/logger/), [logger/MgenLoggerMacro.{h,cpp}](../../code/BasicLibs/core/logger/), [logger/CorrelationContext.{h,cpp}](../../code/BasicLibs/core/logger/) |
| **Metrics** | [metrics/MetricsRegistry.{h,cpp}](../../code/BasicLibs/core/metrics/) |

### Also-touches (F6 가 다른 흐름에서 어떻게 쓰이는지)

| 흐름 | 쓰임 |
|---|---|
| **F1** (Lifecycle) | `initLogger(cfg)` 1회 / `MetricsRegistry::Initialize(9090)` 1회 / `Shutdown()` |
| **F3** (Pipeline) | 각 카메라 thread 시작 시 `CorrelationScope`. `MLOG_*` 광범위. **Disk Defense 코드는 RtspDetectorUnit.cpp 내부에 임베디드** (F3 의 Primary 이지만 F6 책임) |
| **F4** (Event Output) | event/error 메트릭 increment, SocketIO reconnect counter |
| **F5** (GRPC) | grpc 메트릭 (enabled/peer_count/send/recv/success/failed) |
| **F2** (Configuration) | LoggerConfig / NetworkSettings 에서 log file path / metrics port 로드 |
| **모든 흐름** | `MLOG_*` 매크로 |

### Disk Defense 코드 위치 (F3 의 RtspDetectorUnit.cpp 안에 캡슐화)

| 영역 | 위치 |
|---|---|
| 상수 (FRAME_DISK_PATH/FULL_PCT 90%/EMERGENCY_PCT 80%/RETENTION 7일/CLEANUP 1h/EMERGENCY 5min) | [RtspDetectorUnit.cpp:60-65](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L60-L65) |
| `GetFrameDiskUsedPercent` / `GetFrameDiskBytes` (statvfs 헬퍼) | [:68-88](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L68-L88) |
| `CleanupOldFrameDirs(retention_days)` (L2-Regular) | [:93-147](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L93-L147) |
| `EmergencyCleanupIfDiskHigh()` (L2-Emergency) | [:154-219](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L154-L219) |
| IOWorker thread 의 L2-Regular 호출 | [:1293-1305](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1293-L1305) |
| IOWorker thread 의 L2-Emergency 호출 | [:1307-1327](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1307-L1327) |
| IOWorker thread 의 L1 사전 차단 | [:1333-1345](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1333-L1345) |

---

## §3. How

### 3.1 Logger 흐름

```
[F1 Init]
  LoggerConfig cfg;
  cfg.setLogType(File).setLogSaveFile("/.../DetectBase.log").setEnableJson(true);
  initLogger(cfg);
  └─ get_logger(cfg) (function-local static) — singleton 생성

[Runtime]
  MLOG_INFO("CAM[%d] frame=%d", id, n);
  └─ MGEN::logInfo(GetLogString(fmt, ...))     // 매크로
       └─ logInternal(msg, INFO)
            └─ try { get_logger()->log(msg, INFO); } catch(...) { printf 직접 }
                 └─ FileLogger::log(msg, INFO)
                      ├─ if level < CUTOFF: return
                      ├─ if json_enabled: FormatJsonLine(msg, INFO) → log(out)
                      │     └─ {"ts":"<ISO8601>","lvl":"INFO","msg":"...","correlation_id":"..."}\n
                      ├─ else: timeStamp() + prefix + msg + \n → log(out)
                      └─ log(out) {
                           lock; file_stream << out; flush;
                           unlock;
                           reOpen();   // 일정 주기마다 close → open (logrotate 호환)
                         }
```

### 3.2 CorrelationScope RAII 패턴

```
void OnSocketIoMessage(const json& msg) {
    MGEN::CorrelationScope scope { "evt" };  // ctor: 새 ID 발급, prev 보존
    // 이 thread 의 모든 MLOG_* 가 동일 correlation_id 첨부
    ProcessMessage(msg);
}                                            // dtor: prev ID 로 복원

ID 형식: "<prefix>-<unix_ms>-<seq>"
  예: "evt-1714872000123-42"
```

- thread_local `current_correlation_id` 보유
- atomic global `sequence_counter` 로 동일 ms 내 충돌 방지
- nested scope 안전 (이전 값 stack-unwind 복원)

### 3.3 Metrics 흐름

```
[F1 Init]
  MetricsRegistry::Instance().Initialize(9090);
  └─ pImpl 안에 prometheus::Registry 생성
  └─ Exposer("0.0.0.0:9090") 시작 (civetweb backend)
  └─ exposer.RegisterCollectable(registry)

[F1 Init — DETECTOR.cpp:98-152]
  RegisterGauge / RegisterCounter / RegisterHistogram × 18개
    detectbase_dfps_total                   (gauge)
    detectbase_camera_count                 (gauge)
    detectbase_events_total                 (counter)
    detectbase_errors_total                 (counter)
    detectbase_socketio_reconnect_total     (counter)
    detectbase_frame_disk_used_bytes        (gauge)
    detectbase_frame_disk_capacity_bytes    (gauge)
    detectbase_frame_disk_used_pct          (gauge)
    detectbase_imwrite_skipped_total        (counter)
    detectbase_frame_cleanup_deleted_total  (counter)
    detectbase_frame_emergency_cleanup_total(counter, label=type)
    detectbase_grpc_client_enabled          (gauge)
    detectbase_grpc_client_peer_count       (gauge)
    detectbase_grpc_send_total              (counter)
    detectbase_grpc_server_enabled          (gauge)
    detectbase_grpc_recv_total              (counter)
    detectbase_grpc_send_success_total      (counter)
    detectbase_grpc_send_failed_total       (counter)

[Runtime per metric update]
  MetricsRegistry::Instance().IncrementCounter(name, labels, value);
    └─ lock_guard(mtx)
    └─ counter_families[name]->Add(labels).Increment(value)
       (prometheus-cpp 자체 atomic — 그러나 family map 보호용 mtx 가짐)

[Pull 시점]
  Prometheus 서버가 GET http://<host>:9090/metrics
    → Exposer 가 registry 의 모든 family 의 Collect() → text format 반환

[F1 Shutdown]
  MetricsRegistry::Instance().Shutdown();
    └─ exposer.reset() (HTTP 종료. 측정은 계속 가능)
```

### 3.4 Disk Defense 3-Layer 흐름

```
IOWorker thread (per camera unit) loop:
  ├─ opt_item = io_work_queue_->dequeue_wait_for(1s)
  │
  ├─ [L2-Regular] every 1h:
  │     deleted = CleanupOldFrameDirs(retention=7d)
  │     if deleted > 0 → MLOG_INFO + frame_cleanup_deleted_total++
  │
  ├─ [L2-Emergency] every 5min:
  │     if disk_pct >= 80%:
  │       case 1) day_dirs > 1: oldest 부터 1개씩 삭제 (재확인 반복)
  │       case 2) day_dirs == 1 (당일): files mtime 정렬 → 절반 삭제
  │       빈 월/년 폴더 정리
  │       → frame_emergency_cleanup_total{type="day_dir"|"half_files"}++
  │
  ├─ if !opt_item → continue
  │
  ├─ [L1 pre-block] used_pct = GetFrameDiskUsedPercent()
  │     if used_pct >= 90%:
  │       imwrite_skipped_total{reason="disk_full"}++
  │       cool-down 1min WARN log
  │       continue (imwrite 안 함)
  │
  └─ cv::imwrite(opt_item->frame_path, opt_item->image_mat)
       if fail: errors_total{type="imwrite_fail"}++
```

### 3.5 메트릭 갱신 위치 (F1·F3·F4·F5 와의 결합부)

| 메트릭 | 갱신 위치 | 빈도 |
|---|---|---|
| dfps_total | DETECTOR.cpp 통계 thread | 5초 주기 추정 |
| camera_count | DETECTOR.cpp Init | 1회 |
| events_total | F4 emit 시 | 이벤트당 |
| errors_total | 모든 흐름 | 에러 발생 시 |
| socketio_reconnect_total | F4 SioHandler 재연결 시 | 재연결당 |
| frame_disk_*_bytes / used_pct | DETECTOR.cpp 통계 thread | 주기 |
| imwrite_skipped_total | RtspDetectorUnit.cpp IOWorker L1 | skip 시 |
| frame_cleanup_deleted_total | RtspDetectorUnit.cpp IOWorker L2-R | 1h 주기 |
| frame_emergency_cleanup_total | RtspDetectorUnit.cpp IOWorker L2-E | 5min 주기 |
| grpc_* | F5 GrpcEventClientBase / GrpcEventServerBase | RPC 마다 |

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `Logger` (Console/File) | function-local static `unique_ptr<Logger>` in `get_logger()` | 프로세스 종료까지 |
| `MetricsRegistry` | function-local static `MetricsRegistry instance` | 프로세스 종료까지 |
| `MetricsRegistry::Impl` | `unique_ptr<Impl>` 멤버 | Registry 와 동일 |
| `prometheus::Registry` | `Impl::registry` shared_ptr | Impl 종속 |
| `prometheus::Exposer` | `Impl::exposer` unique_ptr | Initialize/Shutdown 사이 |
| `prometheus::Family<T>*` | Registry 가 소유, `Impl::*_families` map 은 raw ptr 만 보유 | Registry lifecycle |
| `current_correlation_id` | thread_local `std::string` | 스레드 lifecycle |
| `CorrelationScope::previous_` | scope 객체 stack | scope 객체 lifecycle |

---

## §5. Concurrency

### Logger
| 보호 대상 | 락 |
|---|---|
| FileLogger 의 `file_stream` | `Logger::lock` mutex |
| FileLogger 의 `last_re_open` / 재오픈 | `Logger::lock` mutex |
| ConsoleLogger 의 `std::cout` | **락 없음** — `std::cout` 자체 thread-safe + 한번에 `<< msg` 1회만 호출하므로 인터리빙 위험 약함 (코멘트로 명시) |

### CorrelationContext
- `current_correlation_id` 는 thread_local — 락 불필요
- `sequence_counter` atomic — relaxed memory order

### MetricsRegistry
- 모든 Register / Increment / Set / Observe 가 `Impl::mtx` 로 직렬화
- prometheus-cpp 의 `Counter/Gauge/Histogram::Add().Increment()` 자체는 atomic 이므로, mtx 는 family map(`unordered_map<string, Family*>`) 의 lookup race 방어용
- 여러 thread 가 동시에 같은 메트릭 update 시 정확히 직렬 — 핫 패스에서 contention 발생 가능. 메트릭 호출 빈도가 ms 단위가 아닌 한 문제 없을 추정

### Disk Defense
- IOWorker thread 가 단일. 큐에서 work 가져올 때마다 L1/L2-R/L2-E 순차 실행
- `last_cleanup` / `last_emergency` / `last_skip_warn` — IOWorker thread 만 접근. 락 불필요
- statvfs / std::filesystem 은 thread-safe (POSIX 보장)
- L2-Emergency 의 `fs::remove_all` 은 매 카메라 IOWorker 마다 실행 가능 → 다중 카메라 환경에서 동시 cleanup 발생 가능. 동일 `/frame` 디렉토리에서 race 가능 (`directory_iterator` + `remove_all`). std::filesystem 은 OS 수준 race 에 대해 안전 보장 없음 — 단, std::error_code 분기로 silent ignore 하므로 실패해도 다음 주기 재시도. ⚠ 검토 항목

---

## §6. Findings

### F-F6-01 — `get_logger()` 가 첫 호출 config 만 사용 — `initLogger` 늦게 호출 시 default config 으로 고착
- **등급**: WARN
- **위치**: [MgenLogger.cpp:328-339](../../code/BasicLibs/core/logger/MgenLogger.cpp#L328-L339)
- **내용**: function-local `static unique_ptr<Logger>` 가 첫 호출 시점의 cfg 로 생성. `MLOG_*` 가 `initLogger` 보다 먼저 호출되면 default LoggerConfig (Console + JSON) 으로 singleton 고착. 이후 `initLogger(file_cfg)` 는 무시.
- **현 영향**: 정적 초기화 단계의 로그가 file 로 안 가고 console 만. 또는 main 진입 직전 init 호출이 보장되어야 함.
- **권장**: `get_logger()` 가 initLogger 호출 후에만 동작하도록 강제 (assert) 또는 동적 reconfigure 지원 추가. 즉시 가치 낮음 (현 코드가 main 시작 즉시 init 호출).

### F-F6-02 — `FileLogger::reOpen()` 에서 `throw runtime_error` 발생 가능
- **등급**: WARN (CLAUDE.md 규칙)
- **위치**: [MgenLogger.cpp:301](../../code/BasicLibs/core/logger/MgenLogger.cpp#L301)
- **내용**: 파일 reopen 실패 시 throw. CLAUDE.md "C++ exceptions 사용 금지" 와 충돌.
- **현 영향**: `logInternal` 의 try/catch 에서 잡혀 stderr 출력. 외부 노출 안 됨. 하지만 코딩 규칙 위반.
- **권장**: throw 제거하고 `file_stream.is_open()` 검사로 대체. 단, FileLogger ctor 에도 throw 가 있어 동일 작업 필요.

### F-F6-03 — `GetLogString` 의 200KB 스택 버퍼
- **등급**: NOTE
- **위치**: [MgenLogger.cpp:383-394](../../code/BasicLibs/core/logger/MgenLogger.cpp#L383-L394)
- **내용**: `char buffer[MAX_LOG_MSG_LEN] = {0,};` → 스택에 200KB 잡고 zero-init. 매 로그 호출마다.
- **현 영향**: 일반 thread 스택 8MB 기본. 200KB 는 부담 가능하지만 한도 내. zero-init 비용은 매 호출.
- **권장**: vsnprintf 만으로 충분하므로 zero-init 제거. 또는 thread_local buffer 로 재사용. 즉시 가치 낮음.

### F-F6-04 — `MAX_LOG_MSG_LEN` 코멘트 오류 ("2048 byte" 인데 실제 200KB)
- **등급**: INFO
- **위치**: [MgenLogger.h:34](../../code/BasicLibs/core/logger/MgenLogger.h#L34)
- **내용**: `constexpr size_t MAX_LOG_MSG_LEN = 204800; /* 2048 byte */` — 코멘트가 실제 값과 100배 차이.
- **권장**: 코멘트 수정 ("200 KB").

### F-F6-05 — `MetricsRegistry::Initialize` 의 `prometheus::Exposer` ctor 가 throw
- **등급**: NOTE (이미 try/catch)
- **위치**: [MetricsRegistry.cpp:75-87](../../code/BasicLibs/core/metrics/MetricsRegistry.cpp#L75-L87)
- **내용**: civetweb bind 실패 시 throw. 이미 try/catch 로 graceful degrade (메트릭 측정만 계속).
- **현 영향**: 적절한 처리됨.
- **권장**: 변경 없음.

### F-F6-06 — `RegisterCounter/Gauge/Histogram` 의 `label_keys` 인자 무시
- **등급**: NOTE
- **위치**: [MetricsRegistry.cpp:103-154](../../code/BasicLibs/core/metrics/MetricsRegistry.cpp#L103-L154)
- **내용**: 인자로 받지만 prometheus-cpp BuildX 에 전달하지 않음 (사용 안 함 주석). prometheus-cpp 는 Add(labels) 시점에만 label 결정 → 사실상 인자 불필요.
- **현 영향**: 호출부가 미리 label_keys 를 명시할 수 있어 의도 표현은 유지. 동작은 정상.
- **권장**: 인자 제거하면 시그니처 단순화 가능. 또는 의도 표현을 위해 그대로 유지. 가치 낮음.

### F-F6-07 — `MetricsRegistry` 모든 측정이 단일 `mtx` 직렬화 — 핫 패스 contention 가능
- **등급**: NOTE
- **위치**: [MetricsRegistry.cpp:158-227](../../code/BasicLibs/core/metrics/MetricsRegistry.cpp#L158-L227)
- **내용**: 등록 보호용 락이 측정 핫 패스에도 적용. 다중 카메라 동시 increment 시 직렬 대기.
- **현 영향**: 현재 메트릭 호출 빈도가 ms 단위 미만이라 미체감. 카메라 수 증가 시 영향 가능.
- **권장**: family map lookup 만 락하고 increment 자체는 락 밖에서 호출 가능 (prometheus-cpp Counter 자체 atomic). 미래 개선 항목.

### F-F6-08 — `EmergencyCleanupIfDiskHigh` 의 다중 IOWorker 동시 실행 race 가능성
- **등급**: NOTE
- **위치**: [RtspDetectorUnit.cpp:154-219](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L154-L219)
- **내용**: 카메라 N대 → IOWorker N개 → 각자 5min 주기로 emergency cleanup 호출 가능. 동일 `/frame` 트리에서 동시 `directory_iterator` + `remove_all` race.
- **현 영향**: 모든 fs 호출에 `std::error_code` 사용 → silent ignore. 다음 주기에서 재시도. 데이터 손상 위험은 낮지만 의도하지 않은 추가 삭제 가능 (한 worker 가 삭제 중인 경로를 다른 worker 가 같이 시도).
- **권장**: 프로세스 수준 file lock 또는 static `std::mutex emergency_mtx` 로 동시 1개만 실행. 카메라 수 적을 때는 영향 미미.

### F-F6-09 — Logger 가 prometheus failure 메트릭을 increment 하지 않음
- **등급**: INFO
- **위치**: [MgenLogger.cpp:341-356](../../code/BasicLibs/core/logger/MgenLogger.cpp#L341-L356)
- **내용**: logInternal try/catch 가 stderr 만 출력. logger 자체의 실패는 어디에도 카운트 안 됨.
- **현 영향**: 운영 중 logger 실패가 외부에서 관측 불가.
- **권장**: catch 절에서 `errors_total{type="logger_fail"}` 같은 카운터 increment. 단, 그 메트릭 호출도 실패할 가능성을 고려해 직접 stderr.

### F-F6-10 — KST hard-coded (UTC+9) — P37 으로 이미 결정됨
- **등급**: INFO (의도된 결정)
- **위치**: [MgenLogger.cpp:73](../../code/BasicLibs/core/logger/MgenLogger.cpp#L73)
- **내용**: `gmt.tm_hour + 9` — KST 고정. 1차 리뷰에서 한국 전용 유지로 기각 결정 (P37).
- **현 영향**: JSON 모드(default true)에서는 ISO8601 UTC 사용 — 한국 의존성 부분만 영향. 한국 외 배포 시에만 문제.

### F-F6-11 — DETECTOR.cpp 의 메트릭 등록 — README 와 개수 일치 검증
- **등급**: INFO (긍정 발견)
- **위치**: [DETECTOR.cpp:98-152](../../code/Main/DETECTOR/src/DETECTOR.cpp#L98-L152)
- **내용**: README 는 17개로 표기, 실제 18개 등록 (grpc_send_failed_total 추가됨). 실제 코드 우선.
- **권장**: README 의 표 수치를 18로 갱신 (작은 문서 정합성 항목).

---

## §7. Open Questions

1. **F-F6-01**: `initLogger` 호출이 main 진입 직후 보장되는지 F1 분석에서 확인 필요. 보장된다면 NOTE 강등 가능.
2. **F-F6-08**: 다중 IOWorker 동시 cleanup 의 실제 문제가 운영에서 관찰됐는지? (안 보였다면 NOTE 유지, 보였다면 mutex 추가.)
3. **F-F6-11**: README 메트릭 개수 17 → 18 갱신할까? (작은 문서 작업)

---

## §8. Self-Check

- [x] Primary 파일 모두 읽음 — Logger 6개 + Metrics 2개 + CorrelationContext 2개
- [x] Disk Defense 코드 위치 file:line 정확 표기 — RtspDetectorUnit.cpp:60-65, 68-88, 93-147, 154-219, 1293-1305, 1307-1327, 1333-1345
- [x] §3 시퀀스 — Logger / CorrelationScope / Metrics / Disk Defense 모두 호출 시퀀스 + 코드 출처
- [x] §4 소유권 — singleton + thread_local + scope-stack 모두 구분
- [x] §5 락·CV — Logger lock, MetricsRegistry mtx, IOWorker thread-local state, std::filesystem race 모두 구분
- [x] §6 Finding — 11건 모두 등급 + 출처
- [x] 추측 표시 — "추정" 표기 (메트릭 갱신 빈도, 다중 IOWorker race 영향)
- [x] Also-touches 라벨 다른 흐름 Primary 와 모순 없음 — Disk Defense 코드는 F3 의 RtspDetectorUnit.cpp 안에 있고 그게 F3 Primary 이지만 F6 에서 also-touches 로 다룸 → 정합

**검증 결과**: PASS

**보강 필요 항목**:
- F1 분석 시: `initLogger` 호출 시점이 모든 정적 초기화 이후, main 첫 줄 직후인지 확인 → F-F6-01 등급 결정
- F3 분석 시: IOWorker thread 가 카메라당 1개인지, 단일 process 내 IOWorker 동시 실행 패턴 확인 → F-F6-08 등급 결정
- F4·F5 분석 시: 메트릭 호출 패턴 — 호출 빈도가 핫 패스인 지점 식별 → F-F6-07 우선순위 결정
