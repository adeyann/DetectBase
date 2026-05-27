# DetectBase

**Version**: `0.1.18` (cmake `code/CMakeLists.txt`, develop 현 HEAD = master tag `v0.1.18` 와 일치). **last released master tag = `v0.1.18` (2026-05-27)**. v0.1.0 (5/19) → v0.1.18 (5/27) 누적 cam stuck fix + `.claude/settings.json` 권한 allow/deny 모델 + audit ASan/TSan 최소 1h 강제 (현 default ASan 4h / TSan 1h). **TeardownPipeline unref-skip on stuck (0.1.18)** — cam 661 의 42분 cam_loss 분석 결과 `gst_object_unref(pipeline_)` 가 GStreamer 내부 thread join 에서 unbounded block 하던 게 root cause. `gst_element_get_state` timeout 시 unref 건너뛰고 의도된 leak (process restart 시 OS cleanup) + WARN log. **5/26~5/27 11.3h 후속 모니터 wd=1 (boot only) / cam_loss=0** (pre-fix 50분 wd=6/cam_loss 영구). 단 fix path 미발화 (자연 stuck 안 일어남) → fix 실제 발휘 검증은 다음 stuck 시점에 가능. **git workflow 정책 갱신 + pre-push docs check 절대 규칙 + memory 영어화 (0.1.17)** — code/cmake bump 분리 (push 후 별도 commit), 머지 시 사용자 버전 확인, post-merge placeholder bump, 모든 commit push 시 docs 전수 점검. AI-only memory 디렉토리 영어 단일 언어화. **argv guard + flock single-instance lock (0.1.16)** — Main.cpp 의 `int main()` 가 argv 무수신 → `--version` 같은 일반 호출이 풀 서비스 spawn 하던 사고 (PID 4924, DFPS 50% 하락) 차단. argv 명시 case 외 FATAL exit 2 + `/DetectBase/logs/.detectbase.lock` 의 `flock(2)` advisory lock 으로 두 번째 instance 부팅 차단. monitor.sh 에 threshold alert 7 종 (storm/err/dfps_low/memory/wd/ftc/cam_loss) + warmup grace 4 cycle 추가. **REST response JSON parse silent catch 가시화 (0.1.15)** — `rest_impl.cpp` 의 catch 에 `MLOG_WARN` 추가. **MPP + Option A 완전 폐기 (0.1.14)** — mppvideodec 미사용 상태에서 Option A 의 14ms partial reset 이 multi-cam cluster sync 강화 → DFPS dip 증폭 확인. 5/24 Full reset baseline (mean DFPS 115.6, ≥110: 98.8%) 복귀. snapshot: tag `mpp-architecture-snapshot-v0.1.13` + `.backup/mpp_purged_20260526/`. **per-cam stage FPS counter (0.1.12) revert (0.1.13)** — global mutex hot path 으로 wd 빈도 증가 회귀, A-B-A control test 검증.)

Odroid M2 NPU 기반 RTSP 비디오 분석 베이스 프로젝트. 객체 탐지 + 트래킹 + 침입 감지 + 이벤트 송신을 통합한 production-ready 시스템.

> **이 README 는 프로젝트 전체 문서**. 빌드/실행/운영/구조/기법/검증까지 한 곳에서 파악 가능.
> 운영 트러블슈팅 / 알림은 [OPERATIONS.md](OPERATIONS.md), 코드 모듈 세부는 [code/README.md](code/README.md).

---

## 목차

1. [프로젝트 개요](#1-프로젝트-개요)
2. [시스템 아키텍처](#2-시스템-아키텍처)
3. [end-to-end 파이프라인](#3-end-to-end-파이프라인-1-frame-의-일생)
4. [기술 스택](#4-기술-스택)
5. [설계 원칙](#5-설계-원칙)
6. [동시성 모델](#6-동시성-모델)
7. [모듈 구성](#7-모듈-구성)
8. [빌드 / 실행](#8-빌드--실행)
9. [설정 시스템](#9-설정-시스템)
10. [감지 이벤트](#10-감지-이벤트)
11. [외부 통신](#11-외부-통신)
12. [로그 / 메트릭](#12-로그--메트릭-처음-보는-사람-가이드)
13. [디스크 방어 정책](#13-디스크-방어-정책-다층)
14. [Debug Virtual Lines](#14-debug-virtual-lines-시연용-임시-코드)
15. [정적 분석 / Sanitizer (audit)](#15-정적-분석--sanitizer-audit)
16. [검증 단계](#16-검증-단계-1차--2차--3차-리뷰--48h-운영-테스트)
17. [분기 프로젝트 가이드](#17-분기-프로젝트-가이드)
18. [관련 문서 인덱스](#18-관련-문서-인덱스)

---

## §1. 프로젝트 개요

### 무엇을 하는가?

- 카메라 (RTSP stream) 의 실시간 영상에서 **사람 / 차량 탐지** → **객체 트래킹** → **경계선 / 영역 침입 감지** → **이벤트 송신**
- 추가로: ONVIF metadata 출력, RTSP 프록시 출력 (분석 결과 video stream)

### 누구를 위한가?

- 분기 프로젝트 (Master/Slave, 도메인 특화 분석 등) 의 **기반 (base)** 으로 fork 해서 사용
- 코드 자체가 분기 시 변경할 부분이 최소화되도록 설계됨

### 무엇이 baseline 인가?

- production-ready 상태 (2026-05 검증 완료, 누적 패치 57건 — §16 의 표 참조)
- 3차 코드리뷰 통과 (자동화 도구 + 운영 시뮬레이션 + 차분 회귀)
- 48h 운영 안정성 검증 (메모리 / FD / Thread leak 없음)

### 하드웨어 환경

- Odroid M2 (aarch64, RK3588 NPU)
- 외부 카메라 4대 (RTSP), 디스크 (frame 저장용), 네트워크 (MVAS 서버 + 선택적으로 다른 노드)

---

## §2. 시스템 아키텍처

```
┌─────────────────────────── DetectBase 프로세스 ───────────────────────────┐
│                                                                          │
│  ┌───── 입력 ─────┐   ┌──── 분석 ────┐   ┌─── 출력 ───┐                   │
│  │ RTSP 카메라 N  │   │  NPU 추론     │   │ SocketIO   │ → MVAS broker     │
│  │ (외부 호스트)  │ → │  (YOLOv5)     │ → │ REST       │ → MVAS API        │
│  │ GstRtsp       │   │  + SORT       │   │ RTSP proxy │ → 외부 viewer     │
│  │ Receiver      │   │  + Abnormal   │   │ GRPC (선택) │ → 다른 노드       │
│  └──────────────┘   └──────────────┘   │ 이미지 저장 │ → /frame 디스크   │
│                                       └────────────┘                    │
│                                                                          │
│  ┌──── 관리 (Manager 계층) ──────────────────────────────────────────┐    │
│  │ NetworkManager    : SocketIO/REST/RTSP/GRPC client 통합              │    │
│  │ IOStreamManager   : 큐 / dispatcher 집중 관리                       │    │
│  │ EngineLoadBalancer: NPU 엔진 부하 분산 (Multi-engine 지원)          │    │
│  │ SettingManager    : 설정 데이터 (Camera/Schedule/Server/Exclude)    │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌──── 횡단 (모든 컴포넌트가 사용) ─────────────────────────────────────┐    │
│  │ MgenLogger       : JSON 구조화 로그 + correlation_id (thread 별)    │    │
│  │ MetricsRegistry  : Prometheus exporter (:9090/metrics)              │    │
│  │ SafeQueue/Thread : backpressure (max_size + drop oldest) + RAII     │    │
│  │ ReplyDispatcher  : UUID 기반 async 응답 매칭                        │    │
│  └────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────┘
```

### 핵심 컴포넌트 표

| 컴포넌트 | 역할 | 위치 |
|---|---|---|
| **Service_DETECTOR** | 최상위 라이프사이클 (Init → Run → Quit) | [code/Main/DETECTOR/](code/Main/DETECTOR/) |
| **RtspDetectorBlock / Unit** | 카메라 1대 = 1 Unit. 흐름 통합 | [RtspDetectorUnit.cpp](code/Main/DETECTOR/src/RtspDetectorUnit.cpp) |
| **EngineHandlerBase / NPU** | NPU 추론 엔진 추상화 + RKNN 구현 | [code/Engine/](code/Engine/) |
| **SORTTracker** | Kalman + Hungarian 기반 객체 트래커 | [code/Tracker/SORT/](code/Tracker/SORT/) |
| **AbnormalActionChecker** | LineIntrusion / AreaIntrusion / Vehicle | [code/AbnormalActions/](code/AbnormalActions/) |
| **NetworkManager / SioHandler / RtspHandler** | 외부 통신 통합 | [code/Management/](code/Management/) |
| **SettingManager** | 4종 설정 데이터 운영 시 live reload | [code/Management/manager/include/](code/Management/manager/include/) |
| **GrpcEventServerBase / ClientBase** | 노드 간 통신 (선택) | [code/Protocol/GRPC/](code/Protocol/GRPC/) |

---

## §3. end-to-end 파이프라인 (1 frame 의 일생)

```
[T+0ms]    카메라 RTSP RTP 송신
              │
              ▼
[T+5ms]    GStreamer rtspsrc + avdec_h264 디코드 (GstRtspReceiver)
              │   AVFrame 생성
              ▼
[T+5ms]    RtspDetectorUnit::avframe_q_ 에 enqueue
              │   (RTSP thread → Unit thread 경계)
              │
              ▼
[T+10ms]   Unit InferenceThread dequeue → 전처리
              │   sws_scale (resize, padding, color convert) + blob 변환
              ▼
[T+30ms]   EngineLoadBalancer::RequestAsync 호출
              │   engine_input_q (max=128) 에 enqueue
              ▼
[T+30ms]   NPU EngineHandler dequeue → rknn_run (~19ms 동기, batch_size=1)
              │   YOLOv5 추론 → object boxes
              ▼
[T+49ms]   PostProcess → infer_respond_q 에 enqueue
              │   ReplyDispatcher 가 unit_id 별로 분배
              ▼
[T+49ms]   Unit RespondAsync 깨어남 (cv::wait)
              │   좌표 변환 → SORTTracker.update() → 추적된 객체
              ▼
[T+54ms]   AbnormalActionChecker → 침입 여부 판단
              │   schedule 적용 (시간 / 요일 / ROI)
              ▼
[T+58ms]   이벤트 발생 시 분기:
              │
              ├─ SocketIO emit → emit_queue (max=1000) → control thread → 송신
              │
              ├─ GRPC broadcast (조건부) → fire-and-forget
              │
              └─ io_work_queue (max=30) → IOWorker thread → cv::imwrite (~50-200ms, 비동기)
                                                                │
                                                                └─ /frame/YYYY/MM/DD/*.jpg
                                                                │
                                                                └─ L1/L2-Regular/L2-Emergency 정책 적용
```

**메인 흐름 latency 약 50-60ms per frame (NPU 19ms + 전후 처리). DFPS 약 29/cam × 4 cam ≈ 115~116** (NPU multi-core PR #6 이후 baseline. 이전 single-core 시점엔 ~13/cam × 4 = ~52)

### 흐름의 핵심 특성

- **NPU 사용량**: `rknn_run` 동기 호출 (~17ms 실측, batch_size=1). NPU 3-core multi-handler (PR #6) 이후 실측 천장 ~140 FPS ([.DOCS/NPU_MODEL_PERFORMANCE.md](.DOCS/NPU_MODEL_PERFORMANCE.md) 참조). 4 cam 환경 baseline ~115~116 inf/sec = NPU 천장의 ~83% 사용. multi-core 도입 전 single-core 시절엔 ~53 inf/sec 으로 NPU 천장 도달. batch_size=1 hard-locked — 6+ cam 확장 시 batch>1 검토 필요.
- **async/ready 분리**: thread 가 큐에서 대기 (block) — busy-wait 없음
- **백프레셔**: 모든 큐가 max_size + drop oldest. NPU 따라가지 못해도 메모리 안 누적
- **IOWorker 비동기**: cv::imwrite 가 50~200ms 걸려도 메인 흐름 영향 없음 (별도 thread)

---

## §4. 기술 스택

| 영역 | 라이브러리 | 버전 | 왜 쓰는가 |
|---|---|---|---|
| **NPU 추론** | RKNN runtime | v1.5.2 | Rockchip RK3588 NPU 의 공식 inference SDK |
| **NPU 모델** | YOLOv5s (Airockchip RKNN 변환) | — | 빠르고 정확한 객체 탐지 (사람/차량) |
| **객체 트래킹** | SORT (Simple Online Realtime Tracking) | 자체 구현 | Kalman 필터 + Hungarian 매칭. 가벼움 |
| **영상 디코딩** | FFmpeg (libav*) | apt | RTSP / RTP / H.264 / H.265 표준 |
| **영상 처리** | OpenCV | apt | resize, color convert, imwrite |
| **RTSP** | GStreamer 1.20.3 (`rtspsrc` + `avdec_h264`) | apt | RTSP 수신 (`GstRtspReceiver`) + Proxy 서버 (`GstRtspProxyServer`) + Client wrapper (`GstRtspClient`) + ONVIF metadata payloader (자체 구현). v0.1.14 부터 happytimesoft 외부 RTSP 라이브러리 폐기 → GStreamer 통합. |
| **REST 통신** | restclient-cpp | 0.5.3 | MVAS API 호출 (카메라/스케줄 조회) |
| **WebSocket 유사 통신** | socket.io-client-cpp | 3.1.0 | MVAS broker 와 이벤트 송수신 (양방향) |
| **RPC** | gRPC + protobuf | v1.30.2 | 노드 간 이벤트/counter 송신 (선택, Master/Slave) |
| **메트릭 exporter** | prometheus-cpp | v1.3.0 | Prometheus 표준 메트릭 endpoint |
| **JSON 파싱** | nlohmann/json | apt | 설정 / 로그 / SocketIO payload |
| ~~**YAML 파싱**~~ | ~~yaml-cpp~~ | — | ~~클래스 정의 (classes.yaml)~~ — **P3 (2026-05-14) 폐기**: vendored yaml-cpp 제거 + ClassChecker → nlohmann::json. 현 classes 파일 = `engine.classes.json` |
| **빌드** | CMake 3.17+ + GCC 11 | apt | aarch64 native |
| **컨테이너** | Docker + docker-compose | — | aarch64 격리 빌드/실행 |
| **로그 회전** | logrotate | apt | **daily + rotate 14** (매일 회전, 14일치 보관, 압축, copytruncate). `scripts/logrotate.detectbase` 참고 |
| **메모리 할당자** | jemalloc | apt (`libjemalloc2`) | LD_PRELOAD + `background_thread:true` + `metadata_thp:auto`. RSS 단편화 회피 + 페이지 자동 회수 (2026-05-14 도입) |

### 외부 라이브러리 정책

- **모든 외부 라이브러리는 컨테이너 내부에서 source build**. 미리 컴파일된 .so 안 씀
- 이유: aarch64 ABI 호환성 (x86 prebuilt 와 다름) + 버전 잠금 (tag lock)
- [Dockerfile.build](Dockerfile.build) 가 git tag 명시적으로 lock

---

## §5. 설계 원칙

### CLAUDE.md 코딩 표준 (요약)

| 항목 | 규칙 | 이유 |
|---|---|---|
| **언어** | C++17 | 표준 보장, RAII / smart pointer |
| **메모리** | RAII + 스마트 포인터 (`unique_ptr` / `shared_ptr` / `weak_ptr`). `new` / `delete` 금지 | leak / dangling 차단 |
| **예외** | **자체 throw 금지** — 반환값 / `std::optional` 사용 | 결정적 흐름, 외부 throw 만 try/catch 로 보호 |
| **형 변환** | `static_cast` / `reinterpret_cast`. C-style cast 금지 | 의도 명확성 |
| **헤더** | `using namespace std` 금지 (header) | namespace 오염 차단 |
| **재귀** | 금지 | stack overflow 방지 |
| **const 정확성** | 필수 | 의도 명시 + 컴파일러 최적화 |
| **명명** | Class `PascalCase` / Function `PascalCase` / Variable `snake_case` | 일관성 |
| **들여쓰기** | tab | (legacy) |
| **주석** | 한국어 | (legacy) |
| **공개 API** | Doxygen 코멘트 | 자동 문서화 |

### 스마트 포인터 종류와 사용 가이드

| 종류 | 의미 | 장점 | 단점 | DetectBase 사용처 |
|---|---|---|---|---|
| **`unique_ptr<T>`** | **단독 소유** (복사 불가, 이동만) | 가장 가벼움 (오버헤드 거의 0), 의도 명확, 컴파일 시점 확정 | 공유 불가 | `detector_block_`, `io_work_queue_`, `MgenLogger 의 logger singleton`, NPU EngineHandler |
| **`shared_ptr<T>`** | **공유 소유** (atomic ref count) | 여러 곳에서 동시 보유, 마지막 사용자 소멸 시 자동 정리 | atomic 연산 오버헤드 (캐시 라인), **순환 참조 위험** | 모든 Manager 계층 (Network/IOStream/EngineLoadBalancer), `RtspDetectorBlock`, `GrpcEventServerBase`, `SafeQueue 인스턴스` |
| **`weak_ptr<T>`** | **비소유 참조** (ref count 영향 X) | 순환 참조 끊음, **dangling 차단** (lock() 으로 안전 확인) | 사용 시 lock() 필요 (overhead 작음) | `GrpcUnaryHandler::server_owner_` ([W-13 fix](.DOCS/CODE_REVIEW_SUMMARY.md): server lifetime > handler lifetime 가정 → server 가 먼저 destroy 돼도 dangling 안 됨) |

#### 어떻게 고르나?

```
이 객체를 누가 소유하나?

소유자 1명 (분명)
  → unique_ptr        (90%)
  
소유자 여러 명 (공유 필요)
  → shared_ptr        (10% 미만 권고 — 진짜 공유가 필요한 경우만)

소유자 아니지만 참조만 (lifetime 의존 안 함)
  → weak_ptr          (순환 참조 + dangling 보호)

비소유 + 짧은 함수 안 (안전 보장됨)
  → raw pointer T*    (단, 코멘트 명시: "weak ref, owner: X")
```

#### 사용 시 주의

- **shared_ptr 남발 X**: 진짜 공유가 필요할 때만. unique_ptr 가 가능하면 우선 사용
- **순환 참조 차단**: A → shared_ptr(B), B → shared_ptr(A) = leak. 한쪽을 weak_ptr 로
- **enable_shared_from_this**: `this` 의 shared_ptr 가 필요한 케이스 (예: `GrpcEventServerBase` — handler 가 server 의 weak_ptr 필요)
- **lock() check**: weak_ptr 사용 시 항상 `if( auto sp = wp.lock() )` 패턴. nullptr 가능

### 동작 원칙

| 원칙 | 어떻게 |
|---|---|
| **lifetime 명시** | shared_ptr 의 소유 관계 코멘트 명시 (`server 가 잡음`, `unit 이 잡음` 등) |
| **백프레셔** | 모든 큐 = max_size + drop oldest. NPU 못 따라가면 oldest 버리고 진행. **메모리 안 누적** |
| **graceful degradation** | 한 카메라 / 한 unit 실패해도 나머지는 진행 (전체 다 죽이지 않음) |
| **종료 순서 검증** | "DO NOT REORDER" 코멘트 + 검증된 순서 (DETECTOR.cpp). UAF 차단 |
| **관측 가능성** | 모든 분기 / 실패 / drop 이 메트릭으로 노출. 운영자가 외부에서 진단 가능 |
| **외부 라이브러리 보호** | catch(...) 로 외부 throw 흡수 (자체 throw 는 0). noexcept 시그니처 정합성 |

---

## §6. 동시성 모델

### Thread 구성 (총 약 37개, 카메라 4대 환경)

| 종류 | 카운트 (4 cam 기준) | 역할 |
|---|---|---|
| main thread | 1 | 부팅 + 종료 대기 + signal handling |
| **InferenceThread** (per camera) | N (=4) | avframe_q dequeue → 전처리 → engine 요청 → 응답 대기 → tracker → abnormal |
| **IOWorker thread** (per camera) | N (=4) | io_work_queue dequeue → cv::imwrite + L2 정기/비상 청소 |
| **RTSP receive thread** (per camera, GStreamer `rtspsrc` + `avdec_h264`) | N (=4) | RTSP packet 수신 + 디코드 → avframe_q enqueue |
| NPU EngineHandler thread | **3** (NPU 3-core, PR #6 multi-handler 이후) | per-core engine_input_q dequeue → rknn_run (동기) → infer_respond_q enqueue. LoadBalancer 가 round-robin 으로 3 handler 분배. |
| ReplyDispatcher thread | 1 | infer_respond_q dequeue → unit_id 별 분배 |
| RTSP server thread (GStreamer `gst-rtsp-server`) | 2~3 | 분석 결과 RTSP proxy 출력 |
| SocketIO emit control thread | 1 | emit_queue dequeue → 외부 broker 송신 |
| SocketIO inbound thread (외부 라이브러리) | 1~2 | 설정 변경 수신 |
| GRPC server / client thread (조건부) | 0~2 | 노드 간 통신 |
| MetricsRegistry exposer (civetweb) | 1~2 | :9090/metrics HTTP 응답 |
| Setting / 기타 보조 thread | ~5 | SettingManager / SocketIO 내부 / FFmpeg 디코드 등 |

**카메라 4대 환경 = 약 37 threads** (실측). 카메라 추가 시 **+3 per camera** (InferenceThread + IOWorker + RTSP receive).

### Queue 구성

| 큐 | max_size | drop 정책 | 위치 |
|---|---|---|---|
| **avframe_q** (per camera) | 2 × fps_limit | drop oldest | RtspDetectorUnit |
| **engine_input_q** | 128 | drop oldest | EngineLoadBalancer |
| **infer_respond_q** | 무제한 (사이즈 작음) | — | EngineLoadBalancer |
| **emit_queue** | 1000 | drop oldest | SioHandler |
| **io_work_queue** (per camera) | 30 | drop oldest | RtspDetectorUnit |
| **GRPC client queue** (조건부) | 무제한 | — | NetworkManager |

### 동기화 메커니즘

- **std::atomic<bool>** : SafeThread 의 `is_running_` flag (lock-free)
- **std::mutex + condition_variable** : SafeQueue 의 dequeue 대기
- **std::shared_mutex** : 설정 데이터 (read 다수, write 드뭄)
- **ReplyDispatcher** : UUID 기반 1:1 async response 매칭 (engine response → 원래 unit 깨움)

### 종료 시퀀스 (graceful shutdown ~10초)

```
SIGINT/SIGTERM
   ↓
###. PROGRAM QUIT START
#00. Stop GRPC Server            (외부 client 의 새 요청 수신 차단 — Phase 2 fix 후 detached thread shared_from_this 안전)
#01. Terminate Engines           (NPU 추론 종료)
#02. Terminate Load Balancer     (engine_input_q dispatcher 종료)
#03. Stop Service Implements     (Detector Block / 모든 Unit thread join)
#04. Stop Network Flow           (SocketIO / REST / GRPC client)
#05. Stop IO Stream Manager      (모든 큐 terminate)
###. PROGRAM QUIT SUCCESS        (이 출력 후 컨테이너 종료)
```

**순서 변경 금지** — [DETECTOR.cpp:391-393](code/Main/DETECTOR/src/DETECTOR.cpp#L391) `!!! DO NOT REORDER !!!` 코멘트 참조. UAF 위험. (SocketIO close 진행 중 RtspDetectorUnit destroy 시 setting callback 이 stale unit 접근.)

---

## §7. 모듈 구성

```
code/
├── BasicLibs/        Tier 1 — 모든 흐름이 사용하는 공통 인프라
│   ├── core/
│   │   ├── logger/       MgenLogger (JSON 한 줄) + CorrelationContext (thread-local trace ID)
│   │   ├── metrics/      MetricsRegistry (Prometheus :9090/metrics)
│   │   ├── parser/       JSON / YAML / XML
│   │   ├── structure/    SafeQueue / SafeThread / ReplyDispatcher
│   │   └── types/        MgenTypes, InferObject, EngineStreamTypes 등
│   ├── profile/          외부 설정 파일 → 내부 객체 (NetworkProfile / EngineProfile / ServiceProfile)
│   └── utils/            string / file / ip / math / UUID
│
├── VisionCommon/      IR/RGB 카메라 컬러 모드 판별 등 비전 공통 헬퍼
├── Tracker/           SORT 기반 객체 트래커 (Kalman + Hungarian)
│
├── Engine/
│   ├── EngineBase/    추론 엔진 추상화 (Builder 패턴, InferenceThreadRunner)
│   └── NPU/           RKNN NPU 빌더 + YoloV5_Torch_Onnx_RKNN_NPU 구현
│
├── Protocol/          외부 통신 (4 가지 채널)
│   ├── REST/          curl + restclient-cpp
│   ├── SocketIO/      sioclient
│   ├── RTSP_GST/      GStreamer 기반 (rtspsrc + avdec_h264) + proxy server + ONVIF payloader. v0.1.14 부터 happytimesoft 폐기 후 자체 통합
│   └── GRPC/          gRPC + protobuf — 분석 6 fix 적용 (shared_ptr handler registry)
│
├── AbnormalActions/   침입/체류 이벤트 검사
│                      LineIntrusion / AreaIntrusion / VehicleIntrusion / VehicleParking
│
├── Management/
│   ├── manager/       SettingManager / NetworkManager / EngineLoadBalancer / IOStreamManager
│   └── worker/        ApiHandler / SioHandler / RtspHandler / EngineClient / SettingMonitor / InferenceCounter
│
├── Main/
│   ├── BASE/          main() + signal handler + logger init
│   └── DETECTOR/      Service_DETECTOR + RtspDetectorBlock + RtspDetectorUnit
│
├── CMakeFinder/       Find*.cmake 스크립트
├── .tool/             컨테이너 내부 빌드 자동화 (BuildScript.sh)
└── CMakeLists.txt     루트 빌드 설정
```

자세한 모듈별 의존성 / 외부 통신은 [code/README.md](code/README.md) 참조.

---

## §8. 빌드 / 실행

### 컨테이너 환경 셋업 (1회)

```bash
./detectbase.sh build      # Docker 이미지 빌드 + init (proto 재생성)
```

### 일반 워크플로우

```bash
./detectbase.sh compile    # C++ 소스 컴파일 (~2분, incremental)
./detectbase.sh start      # 서비스 시작 + 로그 follow
./detectbase.sh stop       # graceful shutdown (~10초)
./detectbase.sh restart    # stop → start
./detectbase.sh logs       # 로그 follow only
./detectbase.sh audit      # 전체 (cppcheck + clang-tidy + ASan/UBSan + TSan)
./detectbase.sh audit --no-tsan          # TSan 제외 (정적 + ASan/UBSan)
./detectbase.sh audit --only <tool>      # 단독 실행 (cppcheck|clang-tidy|asan|ubsan|tsan)
./detectbase.sh prune      # 안 쓰는 도커 리소스 정리
./detectbase.sh all        # build + init + compile + start (처음 셋업)
./detectbase.sh help       # 명령 목록
```

### 흐름 가이드

| 시나리오 | 명령 순서 |
|---|---|
| 처음 셋업 | `./detectbase.sh all` |
| Dockerfile 수정 | `build` → `compile` → `restart` (build 후 init 자동) |
| proto 수정 | `init` → `compile` → `restart` |
| C++ 코드만 수정 | `compile` → `restart` |
| 검증 (큰 변경 후) | `audit` (운영 정지 동반. ASan 4h + TSan 1h 가 default — 합 약 5h) |

### 환경 요구

- Odroid M2 (aarch64, RK3588 NPU)
- `insmod rknpu.ko` → `/dev/dri/renderD129` 존재해야
- `/usr/lib/librknnrt.so` (rknn-toolkit2 1.5.2) 호스트에 설치
- Docker, docker-compose
- 외부 디스크 마운트 (`.env` 의 `IMAGE_ROOT_PATH`, 기본 `/hdd_ext/images`)

---

## §9. 설정 시스템

### 설정 파일

| 파일 | 내용 | 변경 빈도 |
|---|---|---|
| `settings/NetworkSettings.json` | MVAS 서버 / SocketIO / REST / GRPC 활성화 | 환경 셋업 시 |
| `settings/EngineSettings.json` | NPU 엔진 (.rknn 파일 / 클래스) | 모델 변경 시 |
| `engines/*.rknn` | NPU 모델 binary | 모델 변경 시 |
| `engines/engine.profile.json` | YOLO 입력 크기 / threshold | 모델 변경 시 |
| `engines/engine.classes.json` | 탐지 클래스 정의 (사람 / 차량 등) — P3 (2026-05-14) yaml → json 마이그레이션 후 .json 사용 | 분기 시 변경 |
| `.env` | IMAGE_ROOT_PATH / NPU_DEVICE_PATH | 호스트 별 다름 |

### NetworkSettings.json 핵심 필드

```jsonc
{
    "MVAS_IP":           "192.168.1.102",   // MVAS 서버 IP
    "SocketIO_Port":     3333,              // MVAS broker
    "REST_API_Port":     8000,              // MVAS API
    "LocalIP":           "192.168.1.72",    // 이 노드 IP (자동 탐지 가능)

    // GRPC (분기 프로젝트 시 활성화)
    "GRPC_Client_Enabled": false,           // 다른 노드에 push 할 건가
    "GRPC_Peers":          [],              // [{ name, ip, port }, ...]
    "GRPC_Server_Enabled": false,           // 다른 노드로부터 받을 건가
    "GRPC_Server_BindAddress": "0.0.0.0",
    "GRPC_Server_Port":    17019
}
```

### EngineSettings.json 핵심 필드

```jsonc
{
    "Engines": [
        {
            "TagName":        "YoloV5s_Airockchip_RKNN",
            "EngineFilePath": "/DetectBase/engines/yolov5s_airockchip.rknn",
            "ProfilePath":    "/DetectBase/engines/engine.profile.json",
            "ClassesPath":    "/DetectBase/engines/engine.classes.json",
            "Enable":         true
        }
    ]
}
```

### 라이브 설정 변경 (운영 중)

- 카메라 / 스케줄 / 서버 / Exclude 설정은 **MVAS 서버에서 SocketIO** 로 push
- DETECTOR 는 SettingMonitor 가 변경 받아 즉시 적용 (재시작 없음)
- 적용 실패 시 graceful degradation — 부분 실패 unit 은 setting_partial_failure_total 메트릭 증가

---

## §10. 감지 이벤트

| 이벤트 | 코드 | 대상 | 검사 | 추가 데이터 |
|---|---|---|---|---|
| **LineIntrusion** | 202 | 사람 | 경계선 침범 (방향: TwoWay / OneWay) | 객체 ID, 좌표 |
| **AreaIntrusion** | 203 | 사람 | 영역 진입 | 좌표 |
| **VehicleIntrusion** | 209 | 차량 | 경계선 침범 | 객체 ID, 좌표 |
| **VehicleParking** | 210 | 차량 | 영역 체류 (지정 시간 초과) | 체류 시간 (sec) |

### 스케줄 시스템

- 카메라 1대마다 N개의 `schedule_id` 등록 가능
- 각 schedule 마다:
  - `EventCode` (위 코드)
  - `LevelCode`
  - `TimeRange` (Start, Range, Weekly)
  - `Roi` / `ExcludeRoi` / `TwoWayLine` / `OneWayLine` 등
  - `BlackList` (특정 날짜 제외)
  - `NotificationDelaySec` (알림 최소 인터벌, 노이즈 차단)
  - `LoiteringDurationSec` (체류 판정 시간)

---

## §11. 외부 통신

### 4가지 채널 비교

| 채널 | 라이브러리 | 방향 | 용도 | default 상태 |
|---|---|---|---|---|
| **REST API** | restclient-cpp | DetectBase → MVAS | 카메라 / 스케줄 / 서버 설정 조회 (한 번씩) | ON |
| **SocketIO** | sioclient | 양방향 | 이벤트 송신 (out) + 설정 변경 수신 (in) | ON |
| **RTSP Proxy** | GStreamer `gst-rtsp-server` (v0.1.14 자체 통합 후) | DetectBase → 외부 viewer | 분석 결과 video stream | ON |
| **gRPC** | grpc-cpp | 양방향 | 노드 간 이벤트 / counter 송수신 (선택) | OFF |

### REST API

- MVAS API 호출 (curl 기반)
- Phase 1 (부팅) 시 카메라 / 스케줄 / 서버 / Exclude 4종 설정 조회
- 운영 중 SocketIO inbound 가 trigger 하면 다시 호출
- timeout 10s, 실패 시 ApiHandler 가 return false (graceful)

### SocketIO

- MVAS broker (port 3333) 와 양방향
- **이벤트 송신 (emit)**: detection 결과를 외부에 전파
  - emit_queue (max=1000) + drop oldest → emit_control_thread → 송신
  - drop 발생 시 `errors_total{type="emit_drop"}` 증가
- **설정 변경 수신 (listener)**: SchemeUpdate / ExceptionUpdate / ScheduleUpdate 등
  - 받은 이벤트가 REST API call 을 trigger (Interprotocol pattern)
- 자동 재연결 (지수 백오프, max 30s)

### RTSP Proxy

- 분석 결과 (탐지 박스 overlay 등) 를 video stream 으로 출력
- 외부 viewer (예: VLC) 가 `rtsp://<host>:555/<cam_id>` 로 접근 (2026-05-19 PR #12 로 mount path `/cam<id>` → `/<id>`, port 8554 → 555 변경)
- 구현: [code/Protocol/RTSP_GST/src/GstRtspProxyServer.cpp](code/Protocol/RTSP_GST/src/GstRtspProxyServer.cpp) (GStreamer `gst-rtsp-server` 기반, v0.1.14 부터 자체 통합 — 이전 happytimesoft 외부 라이브러리 폐기)

### gRPC (선택, 분기 프로젝트용)

- 노드 간 fire-and-forget 또는 request-response (UUID 매칭)
- 활성화 = `NetworkSettings.json` 의 `GRPC_Client_Enabled` 또는 `GRPC_Server_Enabled` = true
- 양방향 분리 ON/OFF 가능 (Master 는 server only, Slave 는 client only 등)
- 자세한 내용은 §17 분기 프로젝트 가이드

---

## §12. 로그 / 메트릭 (처음 보는 사람 가이드)

### 왜 로그 + 메트릭 둘 다?

| 도구 | 무엇이 / 언제 / 왜 | 누가 본다 |
|---|---|---|
| **로그** | "5월 13일 09:03:21 에 660 카메라에서 LineIntrusion 이 1건 검출됐다" — **사건의 상세 기록** | 디버깅 / 사고 조사 |
| **메트릭** | "지난 5분 동안 평균 DFPS = 53.2" — **시간별 수치 추세** | 알림 / 모니터링 / 대시보드 |

**로그 = 한 사건의 자세한 기록 (텍스트)**, **메트릭 = 시간별 합산 수치 (숫자)**. 둘 다 필요.

---

### 로그 (JSON 한 줄)

#### 형식

```json
{"ts":"2026-05-13T09:03:21.123Z","lvl":"INFO","msg":"event_detected type=LineIntrusion cam=660 count=2","correlation_id":"sys-detector-660"}
```

- **ts** : timestamp (UTC ISO 8601)
- **lvl** : 레벨 — TRACE / DEBUG / INFO / WARN / ERROR
- **msg** : 사람이 읽는 메시지
- **correlation_id** : 같은 작업 / 카메라 / 흐름을 연결하는 ID

#### 왜 JSON 한 줄?

- **기계가 파싱 가능** — `jq` / Elasticsearch / Loki 등 외부 도구가 자동 파싱
- **사람도 한 줄로 읽음** — 한 사건 한 줄
- 텍스트 grep / awk 도 통함 (호환성)

#### correlation_id 란?

같은 작업의 모든 로그 라인을 한 ID 로 연결. 디버깅 시 한 thread / 한 카메라 / 한 요청만 따라가기 가능.

```bash
# 660 카메라의 inference 흐름만
grep '"correlation_id":"sys-detector-660"' logs/DetectBase.log

# IOWorker (이미지 저장) 흐름만
grep '"correlation_id":"sys-io_worker-' logs/DetectBase.log

# SocketIO 인바운드 (설정 변경) 흐름만
grep '"correlation_id":"evt-' logs/DetectBase.log
```

#### 위치 / 보존

- `logs/DetectBase.log` (현재)
- `logs/DetectBase.log.1` ~ `logs/DetectBase.log.14.gz` (자동 회전, logrotate **daily + rotate 14** — 매일 새벽 회전, 14일치 보관, 압축. `scripts/logrotate.detectbase` 설정)
- 백업: `logs/.backup_*.log` (시작 시점에 백업)

---

### 메트릭 (Prometheus)

#### 어떻게 작동하나?

```
DetectBase 프로세스
   │
   ├─ 내부에 MetricsRegistry 가 메트릭 누적 (메모리)
   │
   └─ HTTP endpoint :9090/metrics 노출
            │
            │ Prometheus 서버가 5~15초마다 GET 호출 (pull)
            ▼
        Prometheus 서버 (외부)
            │
            │ 시계열 데이터 저장
            ▼
        Grafana 대시보드 (사람이 본다)
        또는
        Alertmanager (임계점 초과 시 알림)
```

**중요**: DetectBase 는 **push 안 한다**. 외부 Prometheus 가 polling. DetectBase 죽어도 외부는 영향 없음.

#### 메트릭 타입 3가지

| 타입 | 의미 | 예시 |
|---|---|---|
| **gauge** | **현재 값** (오를 수도 내릴 수도) | DFPS, 디스크 사용률, active 카메라 수 |
| **counter** | **계속 증가만** (재시작 시 0으로) | 이벤트 누적, 에러 누적, 송신 누적 |
| **histogram** | **분포 / 구간별 카운트** | 지연 시간 분포 (현재 시스템 미사용) |

차이 예시:
- "지금 DFPS 가 53" → gauge (계속 변함)
- "지난 1시간 동안 LineIntrusion 이 1024건 발생" → counter (누적값 차이로 계산)

#### endpoint 사용

```bash
# 전체 dump (텍스트 포맷)
curl -s http://localhost:9090/metrics

# DetectBase 관련만
curl -s http://localhost:9090/metrics | grep "^detectbase_"

# 특정 항목
curl -s http://localhost:9090/metrics | grep "^detectbase_dfps_total "
```

#### 등록된 메트릭 (전체 ~38개 — core 19 + gst_rtsp_* 15 + onvif_* 3 + correlation_mismatch 1. per-cam stage FPS 5개는 **v0.1.13 revert** — global mutex hot path 회귀로 폐기)

**Core (19)**:

| 이름 | 타입 | 설명 |
|---|---|---|
| `detectbase_dfps_total` | gauge | 모든 카메라 합계 detection FPS |
| `detectbase_dfps{cam}` | gauge | per-camera detection FPS (cam 라벨) |
| `detectbase_camera_count{state}` | gauge | 등록 / 활성 카메라 수 (state="registered" 또는 "active") |
| `detectbase_events_total{type, cam}` | counter | 이벤트 감지 누적 (type = LineIntrusion 등) |
| `detectbase_errors_total{type}` | counter | 에러 누적 (type 종류 아래 참조) |
| `detectbase_socketio_reconnect_total` | counter | SocketIO 재연결 누적 |
| `detectbase_frame_disk_used_bytes` | gauge | /frame 디스크 사용 (bytes) |
| `detectbase_frame_disk_capacity_bytes` | gauge | /frame 디스크 용량 (bytes) |
| `detectbase_frame_disk_used_pct` | gauge | /frame 사용률 (%) |
| `detectbase_imwrite_skipped_total{reason}` | counter | imwrite 사전 차단 (L1, 디스크 90% 도달) |
| `detectbase_frame_cleanup_deleted_total` | counter | L2-Regular 정기 청소 (7일 이전 폴더 삭제) |
| `detectbase_frame_emergency_cleanup_total{type}` | counter | L2-Emergency 비상 청소 (80% 도달) |
| `detectbase_grpc_client_enabled` | gauge | GRPC client 활성 (0/1) |
| `detectbase_grpc_server_enabled` | gauge | GRPC server 활성 (0/1) |
| `detectbase_grpc_client_peer_count` | gauge | 연결된 GRPC peer 수 |
| `detectbase_grpc_send_total{rpc}` | counter | GRPC client 송신 시도 |
| `detectbase_grpc_send_success_total{rpc}` | counter | GRPC client 송신 성공 |
| `detectbase_grpc_send_failed_total{rpc, code}` | counter | GRPC client 송신 실패 |
| `detectbase_grpc_recv_total{rpc}` | counter | GRPC server 수신 누적 |
| `detectbase_setting_partial_failure_total{unit_key, unit_id}` | counter | 설정 reset 부분 실패 (graceful degradation) |
| `detectbase_correlation_mismatch_total{cam_id}` | counter | RspThread 의 frame ordering 검증 실패 (engine response 가 INF 요청 순서와 다름) — TSan race fix 부수 도입 |

**RTSP_GST 모듈 (15)** — v0.1.14 부터 GStreamer 기반 receiver/proxy 추가:
`detectbase_gst_rtsp_bus_message_total / client_enqueue_drop_total / client_reconnect_total / decoded_total / depay_buffer_total / errors_total / frames_total / jb_num_lost / jb_num_pushed / jb_rtx_count / last_frame_age_sec / reconnect_total / reset_attempt_total / rtcp_timeout_total / rtp_in_total`

**ONVIF Metadata (3)** — RTSP proxy 의 ONVIF metadata stream:
`detectbase_onvif_metadata_bytes_total / drops_total / packets_total`

#### `errors_total` 의 `type` 라벨

| type | 의미 |
|---|---|
| `imwrite_fail` | cv::imwrite 실패 (디스크 권한 / IO 에러) |
| `emit_fail` | SocketIO emit 실패 |
| `npu_fail` | rknn_run 실패 |
| `preprocess_fail` | 전처리 실패 |
| `sio_disconnect` | SocketIO 연결 끊김 |
| `engine_input_q_drop` | NPU 입력 큐 max_size 초과로 drop |
| `emit_drop` | emit_queue 큐 초과 drop |
| `io_work_drop` | io_work_queue 큐 초과 drop |
| `logger_fail` | logger 자체 실패 (외부 IO) |
| `setting_callback` | setting callback 실패 |

#### 임계점 (정상 / 주의 / 위험)

| 메트릭 | 정상 | 주의 | 위험 | 대응 |
|---|---|---|---|---|
| `dfps_total` | 카메라수×~29 (예: 4cam → ~115) | < 절반 (~57) | 0 | NPU/RTSP 점검. NPU multi-core (PR #6) 이후 baseline. |
| `camera_count{active}` | = registered | < registered | 0 | RTSP 단절 |
| `errors_total{imwrite_fail}` | 0/min | > 0/min | > 10/min | 디스크 / 권한 |
| `errors_total{emit_drop}` | 0 | > 0 | 빠르게 증가 | SocketIO 누적 |
| `frame_disk_used_pct` | < 80% | 80~90% | ≥ 90% | L1 차단 작동 |
| `socketio_reconnect_total` | 0/일 | 1~5/일 | 지속 증가 | 망 / broker 점검 |

자세한 알림 규칙 (Prometheus alertmanager 형식) 은 [OPERATIONS.md §8](OPERATIONS.md) 참조.

---

### 로그 vs 메트릭 — 어떻게 골라 쓰나

| 상황 | 도구 |
|---|---|
| "어제 660 카메라에서 무슨 이벤트가 있었나" | 로그 grep |
| "지난 24h 동안 LineIntrusion 추세" | 메트릭 (counter rate) |
| "특정 카메라 시퀀스 디버깅" | 로그 + correlation_id |
| "전체 DFPS 가 떨어졌나" | 메트릭 (gauge) |
| "imwrite 가 실패하면 알림 받기" | 메트릭 + alertmanager |
| "왜 실패했나 (스택, 컨텍스트)" | 로그 |

---

## §13. 디스크 방어 정책 (다층)

### 왜 필요한가?

DetectBase 가 이벤트마다 frame 을 `/frame/YYYY/MM/DD/*.jpg` 로 저장. 4 카메라 × 8 fps × 24h × 7일 = 디스크 빠르게 가득 참. 그래서 **3 layer 자동 정리**.

### Layer 구성

```
/frame 디스크 사용률 추이:

  100% ─────────────────────────────────────────  (디스크 가득)
   95% ────────────────────────────────────────
   90% ─━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← L1: imwrite 사전 차단 (저장만 skip, 분석 계속)
   85% 
   80% ─━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← L2-Emergency: 비상 청소 (5분 cool-down)
   ...                                            ▼
   60%                                            오래된 일자 폴더 삭제 / 당일 절반 삭제
   ...                                            ▼
   < L2-Regular 정기 청소 (1시간 1회, 7일 이전 일자 폴더 자동 삭제)
```

### Layer 별 동작

| Layer | 임계 / 주기 | 동작 | 메트릭 |
|---|---|---|---|
| **L2-Regular** | 1시간 주기 | 7일 이전 일자 폴더 삭제 | `frame_cleanup_deleted_total` |
| **L2-Emergency** | 80% 도달 + 5분 cool-down | 과거 일자 폴더 우선 → 당일만 남으면 절반 삭제 | `frame_emergency_cleanup_total{type="day_dir"\|"half_files"}` |
| **L1** | 90% 도달 | imwrite 자체 skip (저장만 안 함, 분석은 계속) | `imwrite_skipped_total{reason="disk_full"}` |

### 다중 IOWorker race 방지

카메라 4대 = IOWorker 4개. 동시에 cleanup 시도 → race. 해결: **static mutex + try_to_lock**. 1개 worker 만 진행, 나머지는 다음 주기에 재시도. (IF-02 fix)

### 디스크 가득 만성 시

- 옵션 1: 디스크 증설
- 옵션 2: `FRAME_RETENTION_DAYS` 7 → 3 으로 줄이기 ([RtspDetectorUnit.cpp](code/Main/DETECTOR/src/RtspDetectorUnit.cpp))
- 옵션 3: imwrite 자체 비활성화 (큰 작업, 분기 결정)

---

## §14. Debug Virtual Lines (시연용 임시 코드)

### 무엇?

운영 환경에서 시연 / 검증을 위해 **모든 카메라에 가상의 line/area schedule 을 강제 주입**. 실제 MVAS 설정 없이도 이벤트가 발생.

### 위치

- 코드: [RtspDetectorUnit.cpp](code/Main/DETECTOR/src/RtspDetectorUnit.cpp) — `AddDebugVirtualLines_REMOVABLE` 함수 + 호출 2곳
- 라인: 6개 (가로 3 + 세로 3, 양방향)
- schedule_id: `99999` (LineIntrusion), `99998` (VehicleIntrusion)
- 24시간 / 모든 요일 / 알림 최소 인터벌 1초

### 제거 (운영 배포 시)

```bash
grep -n "DEBUG VIRTUAL LINES" code/Main/DETECTOR/src/RtspDetectorUnit.cpp
```

4곳 식별 후:
1. Helper 함수 정의 block 전체 제거
2. Init() 안의 lock 범위 조정 (강제 적용 → 조건부)
3. ScheduleSettingData callback 안의 호출 1줄 제거

자세한 제거 절차는 코멘트 안에 표시됨.

---

## §15. 정적 분석 / Sanitizer (audit)

### 5종 도구 — 전체 / 묶음 / 단독 실행 모두 지원 (2026-05-19 갱신)

```bash
# 묶음 모드
./detectbase.sh audit                    # 전체 (cppcheck + clang-tidy + ASan/UBSan + TSan)
./detectbase.sh audit --no-tsan          # TSan 제외 (정적 + ASan/UBSan)
./detectbase.sh audit --with-tsan        # = 전체 (backward compat)

# 단독 모드 — 변경 검증 시 필요 도구만 빠르게
./detectbase.sh audit --only cppcheck    # 정적 (~1분)
./detectbase.sh audit --only clang-tidy  # 정적 (~10분)
./detectbase.sh audit --only asan        # 동적, ASan+UBSan 같은 빌드 (운영 정지, default 4h run)
./detectbase.sh audit --only ubsan       # asan 의 alias
./detectbase.sh audit --only tsan        # 동적, race detection (운영 정지, default 1h run)
```

환경변수 override:
- `ASAN_DURATION_MIN` (default **240분 = 4h**, **최소 60분 강제** — 60 미만이면 60으로 자동 보정): `ASAN_DURATION_MIN=60 ./detectbase.sh audit --only asan`
- `TSAN_DURATION_SEC` (default **3600초 = 1h**, **최소 3600초 강제** — 3600 미만이면 3600으로 자동 보정): `TSAN_DURATION_SEC=3600 ./detectbase.sh audit --only tsan`

### 도구 비교

| 도구 | 카테고리 | 검출 | 비고 |
|---|---|---|---|
| **cppcheck** | 정적 분석 | redundant assignment, null deref, dead code | 빠름, 실행 안 함 |
| **clang-tidy** | 정적 분석 | RAII, dangling, narrowing, modernize | compile_commands.json 활용 |
| **AddressSanitizer (ASan)** | 런타임 sanitizer | use-after-free, buffer overflow, leak | -fsanitize=address |
| **UBSan** | 런타임 sanitizer | UB (overflow, null deref) | -fsanitize=undefined (ASan 과 같이) |
| **ThreadSanitizer (TSan)** | 런타임 sanitizer | data race, lock-order-inversion, double lock | -fsanitize=thread (별도 빌드) |

### 결과 기준 (production-ready baseline) — **2026-05-27 v0.1.18 갱신** (master_logs/v0.1.18/audit_20260527_091456/, ASan 4h + TSan 1h default run)

| 도구 | 자체 코드 결함 (master_logs/v0.1.18/audit_20260527_091456) |
|---|---|
| cppcheck | **59건** — 18건 false positive (`_` dummy + structured binding unused), 9건 자연 정리 예정 (ThreadProfiler 마이그레이션 시 t_xxx_set), 나머지는 unreadVariable/useStlAlgorithm 등 NOLINT (cppcheck syntax quirk 로 일부 effective 안 됨, FP/intent 보존). 5/19 baseline 부터 동일 유지. |
| clang-tidy | **0건 ✅** — PR #9 (audit cleanup, NOLINT 24건) + PR #13 (EngineHandlerBase dtor pure virtual UB 진짜 fix) 으로 30 → 0 도달. 5/19 부터 5/27 baseline 까지 동일 유지. |
| ASan / UBSan | **자체 코드 leak 0건** ✅ — 5/27 4h run 기준 leak 발생지 모두 외부 lib (librknnrt rknn_init 175× + GStreamer gst_element_change_state / g_object_unref / g_malloc 등). startup leak (외부 librknnrt.so / glib init) + runtime leak (GStreamer rtpmanager) 모두 수용. ~1.24 MB 누적 (5/19 baseline 1.21 MB 대비 run time 27× 길어졌음에도 거의 증가 X — rtpmanager 정상 cap) |
| TSan | **자체 코드 진짜 race 0건 ✅** (5/27 1h run 기준, **172 건** WARNING — run time 7.5× 길어진 비례 증가). 우리 코드 race 4종 (SioHandler UAF / InferenceCounter map / RegisterMetricsOnce / SafeQueue shared_ptr ref count) 모두 fix 유지. 잔여는 SIGKILL `mutex destroyed` false positive + GStreamer 내부 callback race (외부 lib) + SafeQueue happens-before 추적 한계 (mutex 정상 작동) |

자체 코드 결함 추가 검출 시 `audit_<timestamp>/summary.txt` 보고서 비교.

### Known long-running leak: GStreamer rtpmanager (수용)

ASan 4시간 long-run (default 240분) + interval `__lsan_do_recoverable_leak_check()` 검출 결과:

- **위치**: `libgstrtpmanager.so` 내부 cleanup 코드 (외부 release lib)
- **호출 경로**: `GstRtspReceiver::TeardownPipeline` → `gst_element_set_state(NULL)` → rtspsrc → rtpbin → rtpsession sources `g_list_free_full` → 각 source `g_object_unref` → rtpmanager 내부 finalize 시 g_malloc 메모리 회수 누락
- **크기**: ~320 byte / EOS reconnect cycle (5분 cycle 기준 ~3.8 KB/h ≈ **~340 MB/year**)
- **영향**: Odroid M2 8GB RAM 기준 plateau noise 안 (12h v8 monitor: RSS 602~657 MB ±55 MB stable)
- **우리 코드 책임**: 없음 — TeardownPipeline 의 state NULL 전환 + 대기 + 정확한 unref 검증 완료. call stack 의 #5~#11 은 GStreamer 내부 finalize chain, #15 (TeardownPipeline) 까지가 우리 책임 범위
- **결정 (2026-05-19)**: **수용**. 외부 lib 내부 leak 으로 우리 코드 수정 불가. ~340 MB/year 는 운영 영향 0
- **v1.0.0 후 재시도 예정**: GStreamer 1.24+ 업그레이드 시도 (현 1.20.3, Ubuntu 22.04 jammy default). 1.22/1.24/1.26 changelog 에서 본 케이스 fix 가 명시 안 됐으나 시도 가치 있음

### TSan fix 적용 내역 (2026-05-19, commit 9690b90)

**진짜 race 4종 → 0**:
1. **SioHandler UAF** (8 double lock + 1 race) — `enable_shared_from_this<SioHandler>` 상속 + 모든 sio::client 콜백을 `weak_ptr` capture lambda 로 변경. SioHandler 소멸 후 callback no-op.
2. **InferenceCounter map race** (1 race) — `counters_` unordered_map 의 Regist/Unregist/AddCount 모두 mutex_ 보호. ThreadRunner 는 snapshot 후 lock 해제.
3. **RegisterMetricsOnce init race** (1 race) — bool flag → `std::call_once`.
4. **SafeQueue shared_ptr ref-count race** (5 race) — RspThread/InfThread 의 `frame = *opt_frame` (copy → atomic add) → `std::move` 로 변경. SafeQueue mutex 자체는 정상.

**Frame ordering 방어 카운터** (신규 metric):
- LoadBalancer 의 round-robin handler 분배 + NPU 처리시간 variance 시 cam_result_qs 응답 순서 ≠ frame 순서 가능성 (correlation_id 매칭 검증 없음).
- RspThread 에 `result.correlation_id != item.correlation_id` 검증 + `detectbase_correlation_mismatch_total{cam_id}` counter + WARN log 추가.
- 실측 후 진짜 fix (per-correlation_id lookup or handler affinity) 결정 예정.

**TSan-only fps=1 conditional patch**:
- TSan build (`__SANITIZE_THREAD__`) 시만 `inference_per_cams_fps_limit=1` 강제. 100x slowdown 환경 packet drop 방지. ASan/Release 영향 0 (compile-time guard).

### TSan 주의

TSan instrumentation 100x 느림. 정상 운영 (4 cam) 에서 packet drop 무한 → graceful shutdown hang.
- **권고**: TSan 빌드는 카메라 1대 + `fps_limit = 1~2` + 30s + SIGKILL
- race report 는 detection 시 즉시 stderr 출력 → graceful 안 거쳐도 잡음

---

## §16. 검증 단계 (1차 / 2차 / 3차 리뷰 + 48h 운영 테스트)

### 1차 코드리뷰 (2026-05-07 이전)

- 단계별 작업 카탈로그 ([.DOCS/CODE_REVIEW_SUMMARY.md](.DOCS/CODE_REVIEW_SUMMARY.md) — 레거시)
- 데드 코드 / C-style cast / 재귀 / signal handler async-signal-safe / 잠재 race 등 기초 정리
- Stage 1~24 (그룹 A/B/C/D/G 처리)
- 분석 1~6 / α~θ / A~J 카테고리

### 2차 코드리뷰 (2026-05-08, 흐름 기반)

- **9개 흐름** (F1 Lifecycle / F2 Configuration / F3 Pipeline / F4 Event Output / F5 GRPC / F6 Observability / I1 RTSP / I2 RKNN / I3 BasicLibs) + 결합부 + 메타
- 모든 소스 파일이 누락 없이 흐름에 N:M 매핑
- 결과: WARN 16건 / NOTE 35건 / INFO 34건
- 후속 패치 32건 적용 (자체 throw 0건, 메트릭 5건 보강, graceful degradation, root cause fix)
- 자세한 내용: [.DOCS/REVIEW2/SUMMARY.md](.DOCS/REVIEW2/SUMMARY.md) (레거시)

### 3차 코드리뷰 (2026-05-09, A+B+D 통합)

- **A 자동화 도구 audit**: cppcheck + clang-tidy 100% + ASan/UBSan + TSan
- **B 운영 시뮬레이션**: baseline + 메트릭 부하 + graceful shutdown + 디스크 cleanup
- **D 차분 회귀**: 32건 패치 cross-check
- 결과: 신규 결함 11건 발견, 10건 fix (1건 외부 라이브러리 정책 보존)
- 자세한 내용: [.DOCS/REVIEW3/SUMMARY.md](.DOCS/REVIEW3/SUMMARY.md) (레거시)

### 트리거 도달 항목 처리 (2026-05-09)

- W-13: `server_owner_` raw ptr → `weak_ptr<GrpcEventServerBase>` (enable_shared_from_this)
- W-06: RenewAfterReset 의 nlohmann throw 방지 (type check 5곳)
- OPERATIONS.md 작성

### 48h 운영 안정성 테스트

- **1차 (2026-05-09 ~ 11)**: 메모리/FD/Thread leak 검증 PASS (ERROR 0). 후반 10h RSS +47 MB 검출 → glibc ptmalloc fragmentation 추정
- **W-14 patch (2026-05-11)**: `malloc_trim(0)` 추가 (emergency cleanup 후 heap 강제 반환). [참고] jemalloc 환경 (아래) 에서는 사실상 no-op — 추후 정리 후보.
- **2차 (2026-05-12 ~ 14)**: W-14 효과 검증 완료 (`logs/test_48h_20260512_094418/ANALYSIS.md`, gitignored)
- **jemalloc 적용 (2026-05-14)**: LD_PRELOAD 방식. `background_thread:true,metadata_thp:auto`. glibc ptmalloc 의 단편화 회피 + page 회수 자동화. 코드 변경 0.

### GStreamer + MPP hardware decoder 시도 (2026-05-14 ~ 15) — **롤백**

- 목표: happytimesoft 외부 RTSP 라이브러리 → GStreamer (`rtspsrc + mppvideodec`) 로 교체. MPP (Rockchip VPU, NPU 와 별도 하드웨어 block) 활용으로 CPU decode 부담 ↓ (NPU inference 부하는 무관).
- PoC 14/14 PASS (avdec_h264 1h sanity: 0.07 KB/reconn)
- 통합 후 매 reconnect 시 RSS leak ~10 MB/reconn 발생 (BISECT 결과 reconnect path 의 BuildSourcePipeline 단계 의심, 정확 element 미식별)
- 모든 진단 도구 한계 (ASan / valgrind / GST_TRACERS / jemalloc prof) → **롤백** 결정
- 시도 코드는 `.deleted/gst_attempt_20260515/` 에 보존, 자료는 [.DOCS/GSTREAMER_DEEP_REVIEW.md](.DOCS/GSTREAMER_DEEP_REVIEW.md), [.DOCS/ONVIF_PAYLOADER_DESIGN.md](.DOCS/ONVIF_PAYLOADER_DESIGN.md) (재작업 시 참고)

### happytimesoft baseline 복원 + sanity (2026-05-15)

- 외부 RTSP 라이브러리 (Protocol/RTSP) + tinyxml 복원
- RtspHandler.cpp 는 DetectBase 용 정리본 사용 (REID/AlphaRover 의존 0건)
- clean rebuild PASS (error 0, warning 0)
- **60min sanity PASS**: DFPS 53.2, camera 4/4 active, ERROR 0. RSS peak VmHWM 568,844 kB 후 jemalloc background_thread 가 −44 MB 회수 → plateau ~525 MB 도달.

### 누적 변경 (57건 패치)

| 단계 | 건수 |
|---|---|
| 2차 리뷰 후속 | 32 |
| 3차 리뷰 후속 | 10 |
| 트리거 처리 (W-06, W-13) | 5 |
| clang-tidy 100% 추가 (NEW-12) | 5 |
| W-14 malloc_trim | 1 |
| P1 libyaml-cpp-dev 중복 제거 (Dockerfile) | 1 |
| P3 ClassChecker YAML→JSON | 1 |
| jemalloc (LD_PRELOAD + MALLOC_CONF) | 1 |
| happytimesoft baseline 복원 (GStreamer 롤백) | 1 |
| **합계** | **57** |

---

## §17. 분기 프로젝트 가이드

### 분기 정의

DetectBase 를 **Master/Slave 또는 도메인 특화** 프로젝트로 fork. 베이스 코드는 그대로 유지하면서 **설정 + 도메인 코드만 변경**.

### Fork 시 주요 변경점

| 영역 | 변경 |
|---|---|
| **NetworkSettings.json** | `GRPC_Client_Enabled` / `GRPC_Server_Enabled` = true, `GRPC_Peers` 채우기 |
| **GrpcEventServerBase post-processor** | Master 의 cross-check / Slave 의 counter 송신 로직 구현 |
| **Heartbeat thread** | 정기 동기화 thread 추가 (선택) |
| **engines/engine.classes.json** | 도메인별 클래스 정의 (사람/차량 외 추가) |
| **AbnormalActions** | 도메인별 이벤트 클래스 추가 (예: Loitering, Counting) |
| **logo / 메시지** | DETECTBASE 문자열 변경 (시연용) |

### Fork 후 stress test 권장

```bash
# 1. 변경 사항 적용 후
./detectbase.sh compile
./detectbase.sh restart

# 2. audit 로 정합성 검증
./detectbase.sh audit --with-tsan

# 3. 운영 60분+ 누적 모니터 (RSS/FD/Threads leak 점검)
nohup ./logs/monitor.sh fork_stress > logs/monitor_fork_stress.out 2>&1 &
# JSONL: logs/monitor_fork_stress.jsonl (per-cam ~74 fields nested, 1분 단위)
# 종료 후 python json.loads + awk/jq 로 baseline vs final 비교

# 4. graceful shutdown 시퀀스 검증
./detectbase.sh stop
grep "PROGRAM QUIT SUCCESS" logs/DetectBase.log
```

### 분기 후 트리거 가능한 항목 (현재 보류 중)

| 항목 | 트리거 |
|---|---|
| F-F5-07 GRPC client closer 실제 path | grpc enable + Master/Slave stress |
| F-F5-08 GRPC peer reconnect | 운영 중 peer down |
| F-F5-09 max_message_size | 대용량 image RPC |
| F-I2-01 rknn_run async | 성능 최적화 필요 시 |
| TSan 카메라 1대 환경 검증 | 분기 시 1회 |
| Debug Virtual Lines 제거 | 분기 시 |
| **GStreamer 1.24+ upgrade (Ubuntu 24.04 base)** | **v1.0.0 후** — rtpmanager runtime leak (~340 MB/year) fix 시도. Dockerfile.build 22.04 → 24.04, GStreamer 1.20.3 → 1.24.x, ASan long-run 재검증 |
| ~~MPP / Option A 재시도~~ | ✅ **2026-05-26 완전 폐기**. mppvideodec leak 회피 위한 architecture 였으나 사용 중단. snapshot `mpp-architecture-snapshot-v0.1.13` + `.backup/mpp_purged_20260526/` 보존. 미래 hardware decoder 필요 시 처음부터 재설계 (cluster sync / GMainContext 격리 함께 해결) |
| ~~audit cleanup PR (clang-tidy 21 + cppcheck 11)~~ | ✅ 완료 (2026-05-19, PR #9 + #13). clang-tidy 30 → 0, cppcheck 63 → 59. |

---

## §18. 관련 문서 인덱스

### 핵심 진입점

| 문서 | 내용 |
|---|---|
| **[README.md](README.md)** | 프로젝트 전체 문서 (이 파일) |
| **[OPERATIONS.md](OPERATIONS.md)** | 운영자용 — 트러블슈팅 / 알림 / 백업 / 운영 체크리스트 |
| **[CLAUDE.md](CLAUDE.md)** | 코딩 표준 (AI 에게 주는 지침) |
| **[code/README.md](code/README.md)** | 소스 트리 구조 + 모듈 의존성 + 검증 상태 |

### 다음 세션 진입점 (Claude/AI 작업 시)

| 문서 | 내용 |
|---|---|
| **[logs/NEXT_SESSION.md](logs/NEXT_SESSION.md)** | post-v0.1.18 release 진입점 — v0.1.18 = cam_loss root cause fix (TeardownPipeline unref-skip) + master_logs 보관 절차 (5/27) + cmake bump 정책 정정 (5/27, 단독 bump commit 금지 + work branch local bump 흡수 방식) |
| **[.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md)** | v2.0.0 multi-engine (Search 등) 도입 가이드 — MAIA 기반, event-driven 패턴, Phase 1-5 ~3-4주 작업 |
| [.DOCS/STUCK_ANALYSIS_cam659_20260520.md](.DOCS/STUCK_ANALYSIS_cam659_20260520.md) | cam 659 stuck 사건 분석 (2026-05-20) + 진단 도구 (PR #16) 활용 절차 — legacy |
| [.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md](.DOCS/MISMATCH_SURGE_ANALYSIS_20260520.md) | `correlation_mismatch` 폭증 분석 (2026-05-20) — delta=10 stable backlog — legacy |
| [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md) | v0.1.0 release 직전 세션 전체 진행 (B3/B4 + audit + TSan race fix + 마무리) — legacy |
| [.DOCS/AUDIT_REPORT_20260519.md](.DOCS/AUDIT_REPORT_20260519.md) | audit 결과 + rtpmanager A 결정 — legacy |
| [.DOCS/NPU_MODEL_PERFORMANCE.md](.DOCS/NPU_MODEL_PERFORMANCE.md) | YOLOv5 s/m/l/x 성능 예측 — legacy |
| [.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md](.DOCS/REVIEW3_COMPLETION_BASELINE_20260513.md) | 3차 코드리뷰 완료 시점 baseline 스냅샷 (legacy) |

### 코드 리뷰 결과

| 문서 | 단계 |
|---|---|
| [.DOCS/CODE_REVIEW_SUMMARY.md](.DOCS/CODE_REVIEW_SUMMARY.md) | 1차 + 후속 적용 카탈로그 — 레거시 |
| [.DOCS/REVIEW2/INDEX.md](.DOCS/REVIEW2/INDEX.md) | 2차 리뷰 진입점 (9개 흐름) — 레거시 |
| [.DOCS/REVIEW2/SUMMARY.md](.DOCS/REVIEW2/SUMMARY.md) | 2차 리뷰 통합 종합 — 레거시 |
| [.DOCS/REVIEW3/SUMMARY.md](.DOCS/REVIEW3/SUMMARY.md) | 3차 리뷰 통합 (자동화 도구 + 운영 + 차분 회귀) — 레거시 |
| [.DOCS/REVIEW3/AUTOMATED_AUDIT.md](.DOCS/REVIEW3/AUTOMATED_AUDIT.md) | 3차 — audit 도구 사용 결과 — 레거시 |
| [.DOCS/REVIEW3/RUNTIME_BEHAVIOR.md](.DOCS/REVIEW3/RUNTIME_BEHAVIOR.md) | 3차 — 운영 시뮬레이션 — 레거시 |
| [.DOCS/REVIEW3/DIFF_REGRESSION.md](.DOCS/REVIEW3/DIFF_REGRESSION.md) | 3차 — 차분 회귀 — 레거시 |
| [.DOCS/PHASE1_CODE_REVIEW.md](.DOCS/PHASE1_CODE_REVIEW.md) | GStreamer Phase 1 code review — 레거시 |
| [.DOCS/BASICLIBS_AUDIT.md](.DOCS/BASICLIBS_AUDIT.md) | BasicLibs 초기 audit — 레거시 |
| [.DOCS/GSTREAMER_DEEP_REVIEW.md](.DOCS/GSTREAMER_DEEP_REVIEW.md) | GStreamer Phase 1 deep review — 레거시 |
| [.DOCS/GSTREAMER_REWORK_PLAN.md](.DOCS/GSTREAMER_REWORK_PLAN.md) | GStreamer Phase 1 rework plan — 레거시 |
| [.DOCS/ONVIF_PAYLOADER_DESIGN.md](.DOCS/ONVIF_PAYLOADER_DESIGN.md) | GStreamer Phase 2 ONVIF design — 레거시 |
| [.DOCS/SESSION_DFPS_LEAK_HUNT_20260518.md](.DOCS/SESSION_DFPS_LEAK_HUNT_20260518.md) | dfps leak hunt session (B1/B2 직전) — 레거시 |

### 빌드 / 환경

| 문서 | 내용 |
|---|---|
| [Dockerfile.build](Dockerfile.build) | 빌드 + 런타임 통합 이미지 (detectbase:1.0) |
| [Dockerfile.analysis](Dockerfile.analysis) | 정적 분석 / sanitizer 이미지 (detectbase:analysis) |
| [docker-compose.yml](docker-compose.yml) | 운영 컨테이너 정의 |
| [detectbase.sh](detectbase.sh) | 통합 관리 스크립트 (build/compile/start/stop/audit) |

---

## 라이선스 / 저작권

(분기 프로젝트에서 결정)
