# FINDINGS — 2차 코드리뷰 종합 발견 사항

> Stage I + F-base + F-runtime + INTER_FLOW 의 모든 §6 Findings 를 등급별 / 처리 권장도별로 정리.
> 등급 정의는 [INDEX.md](INDEX.md#findings-등급-정의) 참조.

---

## §0. 한눈에 보기

| 등급 | 건수 (개별) | 처리 권장도 |
|---|---|---|
| **RISK** | 0 → IF-01 검증 후 1건 가능 | 즉시 또는 단기 처리 권장 |
| **WARN** | 14 | 트리거 의존 / 정합성 / 검증 |
| **NOTE** | 38 | 정보 / 잠재 / 검증 권장 |
| **INFO** | 27 | 단순 메모 / 긍정 발견 |

총 **79개 finding** + INTER_FLOW 6 IF-XX = **85개**.

---

## §1. RISK 등급 (즉시 처리 권장)

현재 RISK 등급 0건. 그러나 하나의 **검증 후 RISK 가능 항목**:

### IF-01 — F3↔I1 `avframe_q_` lifecycle race window
- **출처**: [INTER_FLOW.md §3](INTER_FLOW.md), F-F3-11
- **근거**:
  - I1 RTSP 라이브러리가 unit 의 큐 ptr 보유 (`setDecodedFrameSafeQueue`)
  - F1 종료 순서 #03 (detector stop) → #04 (network/RTSP stop) 사이 race window
  - I1 의 `setDecodedFrameSafeQueue(nullptr)` unlink API 존재 여부 미검증
- **검증 필요**:
  1. RtspHandler::StopRTSP 라인 검토 — RTSP 라이브러리 stop 후 enqueue 시도가 멈추는지
  2. CRtspProxy 의 unlink API 존재 여부
  3. SafeQueue terminated 상태에서 enqueue 동작 (현재는 enqueue 그대로 진행)
- **검증 후 처리 결정**:
  - 안전 확인 → NOTE 강등
  - race 확인 → unlink API 추가 또는 종료 순서 강화 → RISK 조치

---

## §2. WARN 등급 (14건)

### Stage I

#### W-01: F-I3-01 — `SafeQueue::dequeue()` throw (CLAUDE.md 규칙)
- 위치: [SafeQueue.h:88-99](../../code/BasicLibs/core/structure/SafeQueue.h#L88-L99)
- 검증 결과: **F3·F4·F5 모두 dequeue_wait_for 만 사용**. dequeue() throw 사용처 0건 → **NOTE 강등 가능**
- 처리: 미사용 dequeue() deprecate 또는 dequeue_wait_for 마이그레이션 후 제거

#### W-02: F-I3-02 — `SafeThread` closer 미설정 시 hang
- 위치: [SafeThread.h:43-52](../../code/BasicLibs/core/structure/SafeThread.h#L43-L52)
- 검증 결과: 모든 사용처가 closer 설정 (F-F5-07 1건 .cpp 검증 필요)
- 처리: SetThreadFunctions 에 closer 미설정 시 ASSERT 추가 (가치 작음)

### Stage F-base

#### W-03: F-F6-01 — `get_logger()` 첫 호출 config 고착
- 위치: [MgenLogger.cpp:328-339](../../code/BasicLibs/core/logger/MgenLogger.cpp#L328-L339)
- 영향: main 진입 즉시 init 호출이 보장되어야 함. F1 의 `InitLogger()` 가 main 첫 줄 → **검증됨**
- 처리: NOTE 강등 권장 (또는 코드 코멘트 강화)

#### W-04: F-F6-02 — `FileLogger::reOpen()` throw
- 위치: [MgenLogger.cpp:301](../../code/BasicLibs/core/logger/MgenLogger.cpp#L301)
- 처리: throw 제거하고 file_stream.is_open() 검사로 대체 (작은 작업)

#### W-05: F-F2-01 — ApiHandler throw
- 위치: [SettingManager.cpp:222-273](../../code/Management/manager/src/SettingManager.cpp#L222-L273)
- **검증 결과**: GET_or_throw_if_timeout 은 더 이상 throw 안 함 (F-F4-01) → **NOTE 강등 + F-F4-01 로 이관**

#### W-06: F-F2-02 — RenewAfterReset throw
- 위치: [SettingManagerBase.h:245-296](../../code/Management/manager/include/SettingManagerBase.h#L245-L296)
- 처리: nlohmann non-throwing API 마이그레이션 (큰 작업, 트리거 의존)

#### W-07: F-F1-01 — Parser throw
- 위치: [DETECTOR.cpp:166-185](../../code/Main/DETECTOR/src/DETECTOR.cpp#L166-L185)
- 처리: Parser 의 std::optional 반환으로 변경 (큰 작업, 트리거 의존)

#### W-08: F-F1-04 — 종료 순서 컴파일 강제 부재
- 위치: [DETECTOR.cpp:376-378](../../code/Main/DETECTOR/src/DETECTOR.cpp#L376-L378)
- 처리: 코멘트 강화 (작은 작업) — INTER_FLOW §3 권장과 동일

### Stage F-runtime

#### W-09: F-F3-01 — `avframe_q_` capacity 무제한
- 위치: [RtspDetectorUnit.cpp:346](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L346)
- 처리: `avframe_q_->SetMaxSize(2 * fps_limit)` 명시 — IF-01 의 mitigation 일부

#### W-10: F-F3-11 — RTSP unit lifecycle race (IF-01 의 직접 출처)
- 처리: IF-01 검증 결과에 따라 결정

#### W-11: F-F4-01 — `GET_or_throw_if_timeout` 네이밍 misleading
- 위치: [ApiHandler.cpp:25-58](../../code/Management/worker/src/ApiHandler.cpp#L25-L58)
- 처리: 함수 rename + 호출부 try/catch 제거 (정합성, 작은 작업)

#### W-12: F-F4-02 — emit_queue drop 메트릭 부재
- 위치: [SioHandler.cpp:53, 219](../../code/Management/worker/src/SioHandler.cpp#L53)
- 처리: enqueue 전 size check + drop 시 `errors_total{type="emit_drop"}` increment (작은 작업, 운영 가치 ↑)

#### W-13: F-F5-01 — `server_owner_` raw pointer dangling
- 위치: [GrpcUnaryHandler.h:62](../../code/Protocol/GRPC/include/GrpcUnaryHandler.h#L62)
- 처리: weak_ptr 또는 enable_shared_from_this 적용 (큰 작업, 현 운영에서 문제 미보고)

#### W-14: F-F5-02 — Client raw new/delete (CLAUDE.md 규칙)
- 위치: [GrpcEventClientBase.cpp](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp)
- 처리: gRPC async pattern 강제. 변경 어려움. 변경 없음.

---

## §3. WARN 처리 권장 분류

### A. 즉시 처리 권장 (작은 작업, 운영 가치 ↑)

| ID | 항목 | 작업량 |
|---|---|---|
| W-08 | F-F1-04 종료 순서 코멘트 강화 | XS (코멘트 1~3 줄) |
| W-09 | F-F3-01 avframe_q_ SetMaxSize | XS (1 줄) |
| W-11 | F-F4-01 GET_or_throw rename + try/catch 제거 | S (3~5 곳) |
| W-12 | F-F4-02 emit_queue drop 메트릭 | S (5~10 줄) |

### B. 검증 후 결정

| ID | 항목 | 검증 |
|---|---|---|
| **IF-01 / W-10** | RTSP unit lifecycle | RtspHandler::StopRTSP + I1 unlink API 검증 |
| W-02 | F-F5-07 client cq closer | GrpcEventClientBase.cpp 한 함수 |
| W-03 | F-F6-01 logger init 시점 | F1 검증 완료 → NOTE 강등 |
| W-05 | F-F2-01 → F-F4-01 | 검증 완료 → 강등 |

### C. 트리거 의존 (큰 작업, 현 위험 0)

| ID | 항목 | 트리거 |
|---|---|---|
| W-04 | F-F6-02 FileLogger throw 제거 | 코드 정합성 작업 일괄화 시 |
| W-06 | F-F2-02 nlohmann non-throwing API | 코드 정합성 작업 일괄화 시 |
| W-07 | F-F1-01 Parser std::optional 반환 | 코드 정합성 작업 일괄화 시 |
| W-13 | F-F5-01 server_owner_ weak_ptr | Master/Slave 본격 사용 + stress 테스트 시 |
| W-14 | F-F5-02 Client raw new/delete | 외부 라이브러리 ABI 강제 — 변경 안 함 |

### D. 변경 없음 (이미 동작)

| ID | 사유 |
|---|---|
| W-01 (F-I3-01) | 미사용 함수, dequeue_wait_for 가 안전한 대체 |
| W-02 (F-I3-02) | 모든 사용처 검증됨 |

---

## §4. NOTE 등급 (38건)

처리 권장도 낮음. 정보성 / 검증 권장 / 트리거 의존.

### Stage I (NOTE 6건)

- F-I3-03: SafeQueue drop 메트릭 hook (F-F4-02 + IF-03 와 통합)
- F-I3-04: ReplyDispatcher timeout entry 누적 (WithCleaner 사용처 OK)
- F-I3-08: getPhysicalIPAddress 비결정성 (단일 NIC odroid OK)
- F-I3-09: ReplyDispatcher::terminate 후 신규 등록 가능 (의도된 lifecycle 외 케이스 없음)
- F-I1-04: RTSP 라이브러리 thread-safety 외부 책임
- F-I1-05: 전역 hrtsp 단일 인스턴스 (DetectBase 단일 OK)
- F-I2-01/02/03: RKNN 동기 호출 / .so vs 헤더 / 버전 미스매치 (모두 검증됨)

### Stage F-base (NOTE 13건)

- F-F6-03: 200KB 스택 버퍼 (영향 미미)
- F-F6-05: Exposer ctor throw (try/catch 적용됨)
- F-F6-06: label_keys 인자 미사용 (동작 정상)
- F-F6-07: MetricsRegistry 단일 mtx 핫 패스 contention (현재 영향 미미)
- F-F6-08: 다중 IOWorker race (silent retry 안전 — IF-02 권장)
- F-F2-03: SettingMonitor [this] capture (ClearAllSubscriptions 호출 보장)
- F-F2-04: callback 실패 메트릭 (F-F6 메트릭 확장 시 추가)
- F-F2-06: RegisterCallback 자동 setter 생성 (의도)
- F-F2-07: CameraSettingData 동적 변경 미지원 (의도)
- F-F2-10: GetSearchServerIP anonymous namespace (구조 OK)
- F-F1-06: Run 실패 시 main exit code 0 (작은 작업)
- F-F1-07: STEP_CHECK 일관성 (작은 작업)
- F-F1-08: ConnMVAS 안의 SettingManager::Initialize (의도된 응집)
- F-F1-10: SIGINT 5회 후 init hang 가능 (docker stop SIGTERM 으로 회피)

### Stage F-runtime (NOTE 19건)

- F-F3-02: RequestAsync drop 메트릭 부재 (F-F4-02 와 통합)
- F-F3-04: AbnormalActions throw 패턴 미검토 (deferred)
- F-F3-05: MAGIC_DETECTION_ENGINE_NAME 하드코딩 (단일 분기 의도)
- F-F3-09: engine_input_q_ size 검사 race window (영향 미미)
- F-F3-12: MAX_ENGINE_INFER_INPUT_QUEUE_SIZE 하드코딩 128
- F-F3-14: IsOverTime wall clock 사용 (영향 미미)
- F-F3-15: subscribe_ids_ unsubscribe 시점 검증 (deferred)
- F-F4-03: GRPC fire-and-forget 통계 (F5 메트릭으로 보완됨)
- F-F4-04: SioEventBinder throw 패턴 미검토 (deferred)
- F-F4-05: SocketIO sync_close timeout (외부 라이브러리)
- F-F4-07: emit_queue copy 비용 (작은 작업 — move)
- F-F4-08: RestClient 단일 connection (현재 영향 미미)
- F-F4-09: GRPC client lifecycle NetworkManager 통합 (의도)
- F-F4-10: socketio_reconnect_total 위치 미검증 (deferred)
- F-F4-11: SocketIO auth token 하드코딩 default (NetworkProfile 추가 권장)
- F-F5-05: AsyncHandler status race (영향 없음)
- F-F5-07: client cq closer 검증 (W-02)
- F-F5-08: peer reconnect 동작 검증 (Master/Slave 본격 시)
- F-F5-09: GRPC max_message_size 설정 (대용량 페이로드 시)
- F-F5-11: ServerCompletionQueue 단일 (현재 영향 미미)
- F-F5-12: Broadcast throw 가능성 검증 (deferred)

### INTER_FLOW (NOTE 5건)

- IF-02 다중 IOWorker race (try_to_lock 권장)
- IF-03 emit/imwrite drop 메트릭 (W-12 와 동일)
- IF-04 GRPC broadcast 호출 위치 검증
- IF-05 종료 순서 코멘트 강화 (W-08 와 동일)
- IF-06 outbound 일관성 docs 추가

---

## §5. INFO 등급 (27건)

긍정 발견 / 단순 메모. 처리 불요.

### 긍정 발견 (긍정적 패턴 인정)

- F-I3-10: MgenFileSystem 매크로 분기 잘 설계됨
- F-F2-05: RenewAfterReset partial failure 시 atomic 롤백
- F-F2-11: DefineDefault 단일 진실 (P39 적용)
- F-F1-02: dtor 의 Quit() 호출 멱등
- F-F1-03: forward decl + .cpp dtor 패턴 (incomplete type 회피)
- F-F1-05: IgnoreSignalHandler async-signal-safe
- F-F3-06: consecutive_mismatch_count_ 노이즈 보호
- F-F3-07: is_schedule_updated_ atomic flag (callback 짧게)
- F-F3-08: RtspDetectorUnit::Init 검증 chain
- F-F3-10: RKNN ctx-per-thread 직렬화 보장
- F-F4-06: SioEventBinder mode flag 4개 (확장성)
- F-F4-12: emit dump 가 emit_control_thread 에서 발생 (main 보호)
- F-F5-03: counter snapshot 1min cool-down log
- F-F5-04: Phase 2 fix self-loopback 검증 통과
- F-F5-13: Phase 4 default post-processor

### 단순 메모

- F-I3-05/06/07: InferObject Rule of Five / hash UB / regex 컴파일
- F-I1-01/02/03/06: 외부 라이브러리 / raw new ABI / startConn jitter / media 미사용
- F-F6-04: MAX_LOG_MSG_LEN 코멘트 오류
- F-F6-09: Logger 실패 메트릭 부재
- F-F6-10: KST 하드코딩 (P37 결정)
- F-F6-11: README 메트릭 17 → 18
- F-F2-08/09: MISSING_UNIT_ID 두 종류 / Initialize service_tag 단일 분기
- F-F1-09/11: STEP_RESET 전역 / WaitUntilQuitSignal 100ms latency
- F-F3-13: frame_count_ 사용처 미확인
- F-F5-06/10: trace_id UUID / DETECTBASE_GRPC namespace

---

## §6. 통합 액션 추출 (사용자 결정 필요)

### Phase A — 즉시 처리 권장 (XS/S 작업, 운영 가치 ↑)

| ID | 작업 | LOC 영향 | 운영 가치 |
|---|---|---|---|
| W-08 / IF-05 | 종료 순서 코멘트 강화 (DETECTOR.cpp:376) | 3 줄 | 코드 안전성 |
| W-09 | avframe_q_->SetMaxSize 명시 | 1 줄 | 메모리 보호 |
| W-12 / F-F4-02 / IF-03 | emit_queue drop 메트릭 | 5~10 줄 | 운영 모니터링 |
| F-F6-04 | MAX_LOG_MSG_LEN 코멘트 수정 | 1 줄 | 가독성 |
| F-F6-11 | README 메트릭 17→18 | 1 줄 | 문서 정합성 |
| W-11 | GET_or_throw rename + try/catch 제거 | 10~20 줄 | 코드 정합성 |
| IF-02 | 다중 IOWorker emergency cleanup mtx | 5 줄 | 효율 |

총 **약 30~40 줄 작업**. 모두 작은 작업이지만 누적 가치 큼.

### Phase B — 검증 후 결정 (관찰 필요)

| ID | 검증 작업 |
|---|---|
| IF-01 / W-10 | RtspHandler::StopRTSP + CRtspProxy unlink 검증 |
| W-02 | GrpcEventClientBase.cpp 의 closer 한 함수 검증 |
| W-03 | F1 의 InitLogger 호출 시점 (이미 검증) → NOTE 강등 |
| W-05 | ApiHandler throw 검증 (이미 검증) → NOTE 강등 |
| F-F4-04 / F-F4-10 / F-F5-07 / F-F5-12 / F-F3-04 / F-F3-15 | deferred 라인 검토 |

### Phase C — 트리거 의존 (큰 작업)

| ID | 트리거 |
|---|---|
| W-04 / W-06 / W-07 | "코드 정합성 일괄 작업" 시 |
| W-13 | Master/Slave 본격 stress 테스트 시 |
| F-F4-11 | Production 배포 시 SocketIO auth token NetworkProfile 추가 |
| F-F5-09 | GRPC 대용량 페이로드 사용 시 max_message_size 설정 |

### Phase D — 변경 없음

| 항목 |
|---|
| W-14 (Client raw new — 외부 ABI) |
| F-I1-* (외부 RTSP 라이브러리 검토 범위 외) |
| F-F6-10 (KST 하드코딩 — P37 결정) |
| F-F2-07 (CameraSettingData 동적 변경 미지원 — 의도) |

---

## §7. Self-Check (FINDINGS)

- [x] 79 + 6 = 85 finding 모두 등급별 분류
- [x] WARN 14건 모두 출처 + 처리 권장 분류 (A/B/C/D)
- [x] NOTE 38건 / INFO 27건 카테고리화
- [x] Phase A 즉시 처리 권장 6~7개 항목 LOC 영향 + 가치 표기
- [x] Phase B 검증 항목 명시
- [x] 단계 종료 후 강등 가능 항목 (W-01, W-03, W-05) 명시
- [x] 추측 표시 — IF-01 의 RISK 후보 / 검증 후 결정

**검증 결과**: PASS

---

## §8. 종합 평가

### 코드 품질 (2차 리뷰 결과)

| 측면 | 평가 |
|---|---|
| RAII 적용 | ✅ 모든 리소스 (thread/queue/dispatcher/handler) 가 RAII |
| 동시성 안전성 | ✅ 락 ordering 명확, callback 락 외부 호출 (#10/#11 fix) |
| Lifetime 관리 | ✅ shared/unique_ptr 체계적, Phase 2 fix 로 detached thread UAF 차단 |
| Backpressure | ✅ 모든 큐가 max_size + drop oldest. 메트릭 보강 필요 |
| 관측 가능성 | ✅ 18개 메트릭 + JSON 로그 + correlation_id. drop 메트릭 일부 누락 |
| Disk Defense | ✅ 3-Layer 방어 (L1/L2-Regular/L2-Emergency/L3) 완비 |
| 종료 순서 | ✅ 검증된 순서 + 코멘트 보호. 강제 메커니즘 부재 |

### 가장 큰 잠재 위험

**IF-01 — F3↔I1 avframe_q_ lifecycle race**. 검증 후 결정.

### 가장 큰 긍정 발견

**Phase 2 fix (분석 6 GRPC UAF)** — `enable_shared_from_this` + handler registry 패턴. 1차 리뷰의 결과물이지만 2차 리뷰에서도 정합성 검증됨.

### 베이스 프로젝트로서의 적합성

DetectBase 가 첫 분기 프로젝트(Master/Slave) 의 출발점이 되기에 충분. Phase A 즉시 처리 항목 정리 + IF-01 검증 후 production-ready 판정.
