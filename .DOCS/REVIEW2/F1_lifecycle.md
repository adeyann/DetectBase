# F1 — Bootstrap & Shutdown Lifecycle

## §1. Why

프로세스 진입(main) → 모든 서브시스템 초기화 → 정상/비정상 종료까지의 단일 책임 흐름.

핵심:
1. **순서 의존성**: Logger → Metrics → Profiles → Managers → Engines → Network → ServiceBlock → GRPC. 이 순서가 깨지면 의존 객체가 미초기화 상태에서 호출.
2. **신호 처리**: SIGPIPE 무시(소켓 끊김 보호), SIGINT 단계별 정책(초기화 중 5회 무시, 이후 정상 종료).
3. **종료 순서의 UAF 차단**: SocketIO close 와 SettingMonitor callback 진행 사이의 race 회피 — 검증된 종료 순서 유지.

제거 시 잃는 것: 프로세스 시작 불가. 비정상 종료 시 dangling thread / 자원 누수.

---

## §2. Roster

### Primary (F1)

| 파일 | 역할 |
|---|---|
| [Main/BASE/src/Main.cpp](../../code/Main/BASE/src/Main.cpp) | main() 진입. Logger / Service_DETECTOR / signal handler |
| [Main/BASE/include/InitMain.h](../../code/Main/BASE/include/InitMain.h) | 경로 / 버전 / cmake config 호환 inline 헬퍼 |
| [Main/DETECTOR/include/DETECTOR.h](../../code/Main/DETECTOR/include/DETECTOR.h) | `Service_DETECTOR` 클래스 정의 (멤버 포함) |
| [Main/DETECTOR/src/DETECTOR.cpp](../../code/Main/DETECTOR/src/DETECTOR.cpp) | Initialize / Run / Quit / WaitUntilQuitSignal 구현 |

### Also-touches

| 흐름 | 활용 |
|---|---|
| F6 (Observability) | InitLogger() 호출 / MetricsRegistry::Initialize/Shutdown 호출 / 18개 메트릭 등록 |
| F2 (Configuration) | NetworkProfileParser::Parse / EngineProfileParser::Parse / ServiceProfileBuilder::Build / SettingManager::Initialize (NetworkManager::ConnMVAS 안에서) |
| F3 (Pipeline) | EngineLoadBalancer / RtspDetectorBlock / engines_ / IOStreamManager 모두 Init/Start/Stop |
| F4 (Event Output) | NetworkManager (ConnMVAS, InitializeRTSPWithStaticCameraList, CloseNetworkAll) |
| F5 (GRPC) | GrpcEventServerBase 의 unique_ptr 멤버. forward decl + .cpp 에 dtor 패턴 (incomplete type 회피) |
| I3 | shared/unique_ptr / atomic / mutex / condition_variable |

---

## §3. How

### 3.1 main() 진입 시퀀스

```
main():
  1. InitLogger()                                      [F6]
       └─ Release: FileLogger("/DetectBase/logs/DetectBase.log") + ReOpen 28일
       └─ Debug: ConsoleLogger + color
  2. App = make_unique<Service_DETECTOR>()
       └─ ctor: service_profile_ = ServiceProfileBuilder::Build()
                service_version_ = GetApplicationVersion()
  3. SIGPIPE = SIG_IGN (sigaction)                     [소켓 끊김 보호]
  4. RegisterSignalHandler(IgnoreSignalHandler)        [초기화 중 무시]
  5. App->Initialize()
       └─ false 면 App->Quit() 후 return
  6. RegisterSignalHandler(ExitSignalHandler)          [초기화 후 정상 종료 핸들러]
  7. App->Run()
       └─ false 면 App->Quit() 후 return
  8. App->WaitUntilQuitSignal(g_terminate_flag)
       └─ 100ms cv.wait_for 폴링 + atomic flag 체크
  9. return 0
```

### 3.2 Service_DETECTOR::Initialize() — Stage 시퀀스

```
Initialize:
  STEP_RESET (current_stage = 1)
  배너 출력

  ────────────────────────────────────────
  P54 Metrics Init (anon block):
    MetricsRegistry::Instance().Initialize(9090)
    18개 메트릭 등록 (Counter/Gauge/Histogram families)
  ────────────────────────────────────────

  Stage #01: Load Engine Profiles & Settings
    NetworkProfileParser::CheckParsable(ns_path) → 파싱 가능성 사전 검사
    NetworkProfileParser::Parse(ns_path)        → network_profile_
    EngineProfileParser::CheckParsable(es_path)
    EngineProfileParser::Parse(es_path)         → engine_profiles_
    ShowEngineProfiles(engine_profiles_)
    STEP_DONE

  Stage #02: Initialize Core Managers
    network_manager_   = make_shared<NetworkManager>(network_profile_)
    io_stream_manager_ = make_shared<IOStreamManager>()
    load_balancer_     = make_shared<EngineLoadBalancer>(engine_profiles_)
    STEP_DONE

  Stage #03: Load Inference Engines (NPU)
    EngineHandlerBuilder_NPU.BuildHandlers(engine_profiles_, load_balancer_->GetEngineLinker())
    if engines_.empty(): return false
    STEP_DONE

  Stage #04: Connect Network & RTSP
    network_manager_->ConnMVAS(service_name)
      └─ 내부적으로 SettingManager::Initialize(...) 호출 (검증 필요)
      └─ SocketIO 연결 / REST API 접근 가능 상태로 만든다
    network_manager_->InitializeRTSPWithStaticCameraList()
      └─ RtspProxyConfigXmlMaker::Make() → /tmp/rtsp_xml 생성
      └─ RtspHandler::Initialize() (g_rtsp_cfg 채움, parse_buf_init)
    grpc_client_enabled / peer_count 메트릭 1회 update
    STEP_DONE

  ShowSettingManagersTotalInfo()  ← Server / Camera / Schedule / Exclude 출력

  Stage #05: Build Service Blocks
    io_stream_manager_->Ready(service_profile_, network_manager_)
    load_balancer_->StartInferenceCounter()
    for engine in engines_: engine->ActivateEngine()
    SocketIOEventBind()                        ← inbound 이벤트 핸들러 등록
    detector_block_ = make_unique<RtspDetectorBlock>(profile, network, io, balancer)
    detector_block_->BuildServiceUnit(10ms)
    detector_block_->Init(30ms)
    STEP_DONE

  ────────────────────────────────────────
  Phase 3: GRPC Server (조건부)
    if(network_profile_.grpc_server_enabled):
      grpc_server_ = make_unique<GrpcEventServerBase>(bind, port)
      SetSendEventOnlyJsonPostProcesser([](req, rsp) { recv 메트릭 })
      SetSendEventWithImagesPostProcesser([](req, rsp) { recv 메트릭 })
      grpc_server_->Run()
      grpc_server_enabled = 1.0
    else:
      grpc_server_enabled = 0.0
  ────────────────────────────────────────

  "SERVICE START SUCCESS"
  return true
```

### 3.3 Service_DETECTOR::Run()

```
Run():
  rtsp_handler = network_manager_->GetRtspHandler()
  rtsp_handler->RunRTSP()                 ← g_rtsp_cfg.proxy 전체에 startConn + tid_pkt_rx/tid_main 시작
  detector_block_->Start(30ms)            ← 모든 RtspDetectorUnit 의 thread 시작 (F3 진입)
```

### 3.4 Service_DETECTOR::Quit() — 역순 종료 (검증된 순서, UAF 차단)

```
Quit():
  if is_quit_.exchange(true): return true  ← 멱등
  cond_.notify_all()                       ← WaitUntilQuitSignal 깨움

  배너 + STEP_RESET

  #00. GRPC Server stop                    (외부 신규 요청 차단)
       grpc_server_->Stop() → reset()
       Phase 2 fix 덕분에 detached thread 도 shared_from_this 로 안전

  #01. Terminate Engines                   (NPU ctx_ release 트리거)
       for engine in engines_: engine->TerminateEngine()

  #02. Terminate Load Balancer             (engine 큐 / queue dispatcher 정지)
       load_balancer_->Terminate()

  #03. Stop Service Implements             (RtspDetectorBlock → Unit 의 thread 정지)
       detector_block_->Stop()

  #04. Stop Network Flow                   (SocketIO / RTSP / GRPC client close)
       network_manager_->CloseNetworkAll()

  #05. Stop IO Stream Manager
       io_stream_manager_->ClearAll()

  Metrics Shutdown                         (HTTP exposer만 stop, 측정 자체는 동작)
       MetricsRegistry::Shutdown()

  "PROGRAM QUIT SUCCESS"
```

### 3.5 종료 순서 코멘트의 중요성

DETECTOR.cpp:376-378 의 코멘트:

> 종료 순서: 원래 순서 유지 (Stage 15 ClearAllSubscriptions 적용 후 새 순서 재시도 예정).
> 새 순서로 변경 시 SocketIO close 도중 setting callback 진행 중인 상태에서
> RtspDetectorUnit 이 그 후에 destroy 되어 UAF 발생.

**즉**: 현 순서는 F2(SettingMonitor callback) ↔ F4(SocketIO) ↔ F3(RtspDetectorUnit) 의 race 를 막는 검증된 배치. 변경 금지.

### 3.6 Signal handler 정책

```
[init phase]
  IgnoreSignalHandler:
    if ++g_force_exit_count <= 5:
      write(STDERR_FILENO, msg, sizeof-1)        ← async-signal-safe
      return
    else:
      ExitSignalHandler(sig)

[run phase]
  ExitSignalHandler:
    g_terminate_flag.store(true, relaxed)        ← atomic
```

- 모든 비-async-signal-safe 함수(MLOG_*, malloc 등) 회피
- 초기화 도중 5회까지는 무시 (의도되지 않은 Ctrl+C 보호), 이후 종료
- SIGPIPE 는 SIG_IGN — 소켓 send 시 끊긴 peer 로 인한 즉시 종료 차단

### 3.7 WaitUntilQuitSignal

```
while !signal_quit_flag.load():
  unique_lock(mtx_)
  cond_.wait_for(lck, 100ms, [&]{ return is_quit_.load(); })
  if is_quit_ or signal_quit_flag: break
this->Quit()
```

100ms 간격 wake-up → atomic flag 두 개 동시 모니터링.

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `Service_DETECTOR` | main() 의 `unique_ptr<Service_DETECTOR>` | main scope. dtor 에서 Quit() 호출 |
| `service_profile_` | Service_DETECTOR 멤버 (value) | service 종속 |
| `engine_profiles_` | Service_DETECTOR 멤버 (`vector<EngineProfile>`) | service 종속 |
| `network_manager_` / `io_stream_manager_` / `load_balancer_` | shared_ptr (Service 가 보유, F3 의 Block/Unit 도 같은 shared_ptr 받음) | 마지막 reference 까지 |
| `engines_` | `vector<unique_ptr<EngineHandlerBase>>` 멤버 | service 종속 |
| `detector_block_` | `unique_ptr<RtspDetectorBlock>` 멤버 | service 종속. Quit 에서 Stop 만 호출, dtor 에서 free |
| `grpc_server_` | `unique_ptr<GrpcEventServerBase>` 멤버 (forward decl + .cpp dtor) | 활성 시만 instantiate |
| Logger singleton | function-local static unique_ptr | 프로세스 종료까지 |
| MetricsRegistry singleton | function-local static instance | 프로세스 종료까지 |
| `g_terminate_flag` / `g_force_exit_count` | global | 프로세스 |

`grpc_server_` 가 `unique_ptr<incomplete_type>` 으로 헤더에 선언 → 헤더에서 GrpcEventServerBase 의 sizeof 가 필요 없음 → DETECTOR.h 의 컴파일 시간 단축. dtor 만 .cpp 로 옮겨 complete type 이 보이는 시점에 destruct 가능 — 이 패턴이 1차 리뷰의 incomplete type 컴파일 오류 해결책.

---

## §5. Concurrency

| 영역 | 보호 |
|---|---|
| `is_quit_` exchange | atomic compare-exchange. Quit 멱등성 보장 |
| `g_terminate_flag` | atomic. signal handler 와 main thread 사이 |
| `g_force_exit_count` | `volatile sig_atomic_t` (async-signal-safe) |
| `mtx_` + `cond_` | WaitUntilQuitSignal 의 100ms polling. cv.wait_for + predicate |
| Signal handler | 오직 async-signal-safe API (write, atomic flag, sig_atomic_t) 만 사용 |

shutdown thread 안전성:
- Quit 은 main thread + dtor 에서 호출 가능. exchange 멱등으로 중복 차단
- 종료 순서는 thread 가 join 되는 순서를 명시: GRPC → engines → balancer → detector_block → network → io. 각 stop 이 자기 thread join 까지 완료한 뒤 다음 stage.

---

## §6. Findings

### F-F1-01 — `NetworkProfileParser::Parse` / `EngineProfileParser::Parse` 가 throw → main 의 Initialize 에서 try/catch
- **등급**: WARN (CLAUDE.md 규칙)
- **위치**: [DETECTOR.cpp:166-185](../../code/Main/DETECTOR/src/DETECTOR.cpp#L166-L185)
- **내용**: 두 Parser 가 throw. `Service_DETECTOR::Initialize` 가 try/catch 로 감쌈. CLAUDE.md "C++ exceptions 사용 금지" 와 충돌.
- **현 영향**: 외부 라이브러리(nlohmann) throw 보호 차원. `CheckParsable` 으로 사전 검사 후 Parse 하므로 실제 throw 가능성 낮음.
- **권장**: Parser 가 `std::optional` 반환하도록 변경 (큰 작업). 즉시 가치 낮음. F-F2-01/02 와 같은 트레이드오프.

### F-F1-02 — `Service_DETECTOR::~Service_DETECTOR()` 에서 `this->Quit()` 호출 — 정상이지만 멱등 의존
- **등급**: NOTE (긍정 발견)
- **위치**: [DETECTOR.cpp:79-82](../../code/Main/DETECTOR/src/DETECTOR.cpp#L79-L82)
- **내용**: dtor 에서 Quit. Quit 은 `is_quit_.exchange(true)` 로 멱등. main 이 정상적으로 Quit 호출했어도 dtor 에서 두 번째 호출은 즉시 return.
- **권장**: 변경 없음.

### F-F1-03 — `unique_ptr<GrpcEventServerBase>` forward decl + out-of-line dtor 패턴
- **등급**: INFO (긍정 발견)
- **위치**: [DETECTOR.h:42](../../code/Main/DETECTOR/include/DETECTOR.h#L42), [DETECTOR.cpp:79](../../code/Main/DETECTOR/src/DETECTOR.cpp#L79)
- **내용**: 헤더는 grpc_server_ 의 sizeof 불필요. cpp 에서 GrpcEventServerBase 가 complete type 일 때 dtor 가 instantiate.
- **현 영향**: 헤더 의존성 격리 + 컴파일 시간 단축. 1차 리뷰의 컴파일 오류 fix 결과물.

### F-F1-04 — Quit 종료 순서 변경 시 UAF 위험 — 코멘트로 명시되어 있지만 컴파일 시점 강제 없음
- **등급**: WARN
- **위치**: [DETECTOR.cpp:376-378](../../code/Main/DETECTOR/src/DETECTOR.cpp#L376-L378)
- **내용**: 코멘트에 "새 순서로 변경 시 UAF" 경고. 향후 작업자가 코멘트 못 보고 변경할 위험.
- **권장**: 종료 순서를 unit test 또는 정적 검사로 강제하기는 어렵지만, 코드 가까이에 더 강한 표시(예: `// !!! DO NOT REORDER !!!` ASCII art) 또는 별도 docs 링크. 작은 작업.

### F-F1-05 — `IgnoreSignalHandler` 의 `++g_force_exit_count <= 5` 는 race-free
- **등급**: INFO (긍정 발견)
- **위치**: [Main.cpp:99-109](../../code/Main/BASE/src/Main.cpp#L99-L109)
- **내용**: `volatile sig_atomic_t` + signal handler 내 `++` 는 async-signal-safe. write(2) 도 async-signal-safe. 모든 사용 API 안전.
- **권장**: 변경 없음. 좋은 패턴.

### F-F1-06 — `Service_DETECTOR::Run` 실패 시 main 에서 Quit() 호출하지만 return 값 처리 없음
- **등급**: NOTE
- **위치**: [Main.cpp:57-58](../../code/Main/BASE/src/Main.cpp#L57-L58)
- **내용**: `if( !App->Run() ) return App->Quit();` — Run 실패 시 Quit() 호출하지만 Quit 은 항상 true 반환. 즉 main 이 0 으로 종료. 비정상 종료를 0 으로 보고하는 셈.
- **현 영향**: docker-compose 가 exit code 로 health check 시 misleading.
- **권장**: `if(!Run()){ Quit(); return 1; }` 로 변경. 가치 작음.

### F-F1-07 — `Service_DETECTOR::Initialize` 의 stage 마다 STEP_CHECK 가 모두 적용되지는 않음 — 일관성 부족
- **등급**: NOTE
- **위치**: [DETECTOR.cpp:84-342](../../code/Main/DETECTOR/src/DETECTOR.cpp#L84-L342)
- **내용**: 일부는 STEP_CHECK, 일부는 if-return-false 직접 분기. 매크로와 직접 코드 혼재.
- **현 영향**: 가독성 저하. 동작은 동일.
- **권장**: 매크로 통일. 가치 작음.

### F-F1-08 — Stage #04 의 SettingManager::Initialize 호출 위치가 NetworkManager::ConnMVAS 안에 숨겨져 있음
- **등급**: NOTE
- **위치**: [DETECTOR.cpp:229](../../code/Main/DETECTOR/src/DETECTOR.cpp#L229) — `network_manager_->ConnMVAS(service_name)`
- **내용**: 외부에서 보면 ConnMVAS 가 단순 connect 같지만, 내부에서 SettingManager::Initialize → REST API 4개 호출 포함. F4 NetworkManager 분석에서 검증 필요.
- **현 영향**: 호출 시점 / 실패 처리가 ConnMVAS 안에 캡슐화. 외부 테스트 어려움.
- **권장**: ConnMVAS 와 SettingManager::Initialize 호출을 명시적으로 분리하는 옵션도 있지만, 응집도 측면에서 현재가 더 자연스러울 수 있음. 검증 후 결정.

### F-F1-09 — `STEP_RESET` 매크로가 `current_stage = 1` 하드 리셋 (전역 변수)
- **등급**: INFO
- **위치**: [MgenLoggerMacro.h:12](../../code/BasicLibs/core/logger/MgenLoggerMacro.h#L12)
- **내용**: `extern int current_stage;` (전역 int). Init 시 1, Quit 시 1. Init 과 Quit 이 같은 thread 에서 호출되므로 race 없음. 다른 thread 가 STEP_* 매크로 사용 시 race.
- **현 영향**: STEP_* 는 main thread (Init/Quit) 에서만 사용. 다른 곳에서 사용 안 함 (검증 필요).
- **권장**: thread_local 또는 atomic 으로 변경 가능하지만 사용 패턴이 main 전용이면 불필요.

### F-F1-10 — IgnoreSignalHandler 가 5회 후에도 ExitSignalHandler 호출만 — 종료 보장 안 됨
- **등급**: NOTE
- **위치**: [Main.cpp:107-108](../../code/Main/BASE/src/Main.cpp#L107-L108)
- **내용**: 5회 이상 SIGINT 시 g_terminate_flag = true. 그러나 main() 의 Initialize 가 wait_for 가 아니라 sequential init 진행 중이라면 그 단계가 끝나야 flag 검사. 즉 init 단계가 hang 이면 SIGINT 무한 호출해도 종료 안 됨.
- **현 영향**: ApiHandler::GET_or_throw_if_timeout 같은 timeout 보호가 있는 곳은 OK. 그 외 hang 발생 가능 단계는 검증 필요.
- **권장**: init 도중 hang 의심점에 timeout 추가 또는 docker stop 의 SIGTERM 으로 강제. 현재 동작이 의도된 것이면 문서화.

### F-F1-11 — `WaitUntilQuitSignal` 100ms polling 은 종료 신호 latency
- **등급**: INFO
- **위치**: [DETECTOR.cpp:432-443](../../code/Main/DETECTOR/src/DETECTOR.cpp#L432-L443)
- **내용**: cv.wait_for 100ms + condition predicate. signal handler 가 atomic flag 만 set 하므로 cv.notify_one 이 없음 → 최악 100ms 대기.
- **현 영향**: 100ms 종료 latency. 사실상 무시 가능.
- **권장**: 변경 없음.

---

## §7. Open Questions

1. **F-F1-08**: ConnMVAS 안에 SettingManager::Initialize 가 숨어있는 것이 의도인가, 분리해야 하는가? F4 분석 후 결정.
2. **F-F1-04**: 종료 순서 보호를 더 강화할까? (코멘트만으로 충분 vs 정적 검사)
3. **F-F1-06**: Run 실패 시 main exit code 1 로 변경?

---

## §8. Self-Check

- [x] Primary 파일 4개 (Main.cpp, InitMain.h, DETECTOR.h, DETECTOR.cpp) 모두 읽음
- [x] §3 시퀀스 — main 진입 / Initialize / Run / Quit / Wait / Signal handler 모두 코드 출처
- [x] §4 소유권 — unique_ptr / shared_ptr / forward decl 패턴 모두 명시
- [x] §5 동시성 — atomic / sig_atomic_t / mutex+cv / async-signal-safe 모두 구분
- [x] §6 Finding 등급 + 출처
- [x] 추측 표시 — "검증 필요" (ConnMVAS 의 SettingManager::Initialize 호출, STEP_* 의 main thread 전용)
- [x] Also-touches 라벨 모순 없음

**검증 결과**: PASS

**보강 필요 항목**:
- F4 분석 시: NetworkManager::ConnMVAS 내부에서 SettingManager::Initialize 호출 시점 / 실패 처리 검증 → F-F1-08 결정
- F3 분석 시: STEP_* 매크로 사용처가 main thread 전용인지 → F-F1-09 검증
- F4 분석 시: GRPC client (NetworkManager 안의) 의 종료 시점 검증 → 종료 순서 정합성
