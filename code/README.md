# DetectBase — Source

DetectBase 의 C++ 소스 트리. 빌드와 실행은 상위 경로의 `detectbase.sh` + Docker 환경에서 수행한다.

## 모듈 구조

```
code/
├── BasicLibs/        Tier 1 — logger / metrics / parser / 공통 타입 / profile / utils
│   └── core/
│       ├── logger/         MgenLogger (JSON 한 줄), CorrelationContext (thread-local trace ID)
│       ├── metrics/        MetricsRegistry (Prometheus exporter, port 9090)
│       ├── parser/         JSON / YAML / XML
│       ├── structure/      SafeQueue / SafeThread 등
│       └── types/          MgenTypes, EngineStreamTypes 등
├── VisionCommon/     IR/RGB 카메라 컬러 모드 판별 등 비전 공통 헬퍼
├── Tracker/          SORT 기반 객체 트래커 (Kalman + Hungarian)
├── Engine/
│   ├── EngineBase/   추론 엔진 베이스 클래스 + 빌더 인터페이스
│   └── NPU/          RKNN NPU 빌더 + YoloV5 RKNN 구현
├── Protocol/
│   ├── REST/         curl + restclient-cpp
│   ├── SocketIO/     sioclient
│   ├── RTSP_GST/     GStreamer 기반 RTSP receiver (rtspsrc + avdec_h264) + proxy server (Full reset architecture, v0.1.14 부터 MPP/Option A 폐기)
│   └── GRPC/         gRPC + protobuf — 분석 6 fix (shared_ptr handler registry) 적용
├── AbnormalActions/  침입/체류 이벤트 검사 (Line/Area Intrusion, Vehicle Intrusion/Parking)
├── Management/
│   ├── manager/      SettingManager, NetworkManager (GRPC client 통합), EngineLoadBalancer, IOStreamManager
│   └── worker/       ApiHandler, SioHandler, RtspHandler, EngineClient, SettingMonitor, InferenceCounter (메트릭 update)
├── Main/
│   ├── BASE/         main() + signal handler + logger init
│   └── DETECTOR/     Service_DETECTOR + RtspDetectorBlock + RtspDetectorUnit (이벤트 emit + GRPC push)
├── CMakeFinder/      Find*.cmake 스크립트
├── .tool/            컨테이너 내부 빌드 자동화 (BuildScript.sh)
└── CMakeLists.txt    루트 빌드 설정
```

## 의존성 / 환경

- C++17, CMake 3.17+
- aarch64 (Odroid M2 / RK3588)
- 외부 라이브러리 — 모두 컨테이너 안에서 source build (Dockerfile.build, tag 락 적용):
  - RKNN runtime `v1.5.2` (rockchip-linux/rknpu2)
  - restclient-cpp `0.5.3`
  - socket.io-client-cpp `3.1.0`
  - gRPC `v1.30.2` + protobuf
  - prometheus-cpp `v1.3.0`
- 분석 도구 (선택, [Dockerfile.analysis](../Dockerfile.analysis)): clang-tidy + cppcheck + clang (ASan/UBSan/TSan 빌드용)

## 빌드

소스만으로는 빌드하지 않는다. 상위 경로의 `detectbase.sh build` / `compile` 가 Docker 컨테이너 안에서 `.tool/BuildScript.sh` 를 호출.

```bash
./detectbase.sh build      # 이미지 빌드 + init (proto 재생성)
./detectbase.sh init       # proto 재생성만 (proto 변경 시)
./detectbase.sh compile    # C++ 컴파일만
```

컨테이너 내부에서 직접 빌드 (디버깅 등):

```bash
bash /DetectBase/code/.tool/BuildScript.sh
```

## 감지 이벤트

DETECTOR 분기에서 활성인 EventClass:

| 이벤트 | 코드 | 대상 | 검사 |
|---|---|---|---|
| LineIntrusion | 202 | 사람 | 경계선 침범 |
| AreaIntrusion | 203 | 사람 | 영역 침입 |
| VehicleIntrusion | 209 | 차량 | 경계선 침범 |
| VehicleParking | 210 | 차량 | 영역 체류 |

## 외부 통신 채널

| 채널 | 라이브러리 | 용도 | 활성 default |
|---|---|---|---|
| REST API | restclient-cpp | MVAS 카메라/스케줄 조회 | ON |
| SocketIO | sioclient | MVAS 이벤트 송신 + 설정 변경 수신 | ON |
| RTSP Proxy | GStreamer `gst-rtsp-server` (v0.1.14 부터 RTSP_GST 자체 통합) | 분석 결과 video stream 출력 | ON |
| **GRPC (옵션)** | grpc-cpp | 노드 간 이벤트 / counter sync | OFF (NetworkSettings 로 활성화) |

GRPC 활성화 / 운영 정책은 [상위 README §"GRPC 통신"](../README.md) 참고.

## 코딩 규칙 (CLAUDE.md 일치)

| 항목 | 규칙 | 예시 |
|------|------|---------|
| Class | PascalCase | `DataManager` |
| Function | PascalCase | `GetData` |
| Variable | snake_case | `data_count` |
| Indent | tab | — |
| Comments | Korean | `// 데이터 초기화` |

**금지**:
- raw `new`/`delete` (RAII + smart pointer 사용)
- C-style cast (`static_cast` / `reinterpret_cast` 사용)
- `using namespace std` (헤더에서 금지)
- 재귀
- C++ 예외 (반환값 기반 에러 처리)

**원칙**:
- C++17, RAII 강제, 예외 안전성 보장
- 명시적 ownership (`unique_ptr` / `shared_ptr`)
- const 정확성 필수
- 공개 API 에 Doxygen 주석

## 검증 상태 (2026-05-28 — develop cmake = 0.1.26 작업 진입. `master_logs/v0.1.18/audit_20260527_091456/` baseline 유지, last master tag = v0.1.18. baseline 은 strict mode = ASan 4h + TSan 1h. 2026-05-28 audit 강도 모드 도입 — develop/내부 검증은 light default ASan 1h + TSan 1h. **0.1.26 light audit (ASan 1h + TSan 1h) 검증 통과**: ASan leak 분포 baseline 동등 (1.22 MB / 10,639 alloc — baseline 1.24 MB / 11,515 alloc, 시간 1/4 임에도 거의 동일 = startup+cycle dominated), TSan SEGV 0 (직전 audit 의 OnJitterStatsTimer UAF 결정적 fix 확인) + WARNING 158 (baseline 172, 우리 코드 race 0건 유지).)

| 항목 | 결과 |
|---|---|
| 자체 throw | 0건 (CLAUDE.md 100% 준수) |
| C-style cast 자체 코드 | 0건 |
| 자체 코드 ASan/UBSan 검출 | 0건 (외부 librknnrt.so init leak + GStreamer rtpmanager runtime leak — 모두 수용) |
| 자체 코드 TSan 진짜 race | **0건 ✅** (4 root cause 모두 fix: SioHandler UAF, InferenceCounter map, RegisterMetricsOnce init, SafeQueue shared_ptr ref) |
| 자체 코드 cppcheck 결함 | **59건** (false positive + Profiler 자연정리 + cppcheck syntax quirk suppress 보존) |
| 자체 코드 clang-tidy warning | **0건 ✅** (PR #9 NOLINT 24 + PR #13 EngineHandlerBase dtor pure virtual UB 진짜 fix) |
| graceful shutdown | 10초, PROGRAM QUIT SUCCESS |
| 운영 leak (RSS/FD/Thread) | 0건 (10h sanity baseline: RSS plateau ±20MB, FD/Threads stable) |

### v0.1.13 ~ v0.1.18 변경 (5/26)
- v0.1.13: per-cam stage FPS counter (0.1.12) revert — global mutex hot path 회귀
- v0.1.14: MPP + Option A 완전 폐기 — Full reset 복귀 (5/24 baseline)
- v0.1.15: REST `get_json_from_resp_body` silent catch → MLOG_WARN 추가 (운영 가시화)
- v0.1.16: `Main.cpp` argv guard + `flock(2)` single-instance lock — PID 4924 사고 재발 방지 (Main.cpp 가 argv 무수신이라 `--version` 같은 호출이 풀 서비스 spawn → NPU 양분 → DFPS 50% 하락). 부수: monitor.sh 에 threshold alert 7 종 (storm/err/dfps_low/memory/wd/ftc/cam_loss) + boot ramp warmup grace 4 cycle.
- v0.1.18: `GstRtspReceiver::TeardownPipeline` 의 `gst_object_unref(pipeline_)` 가 GStreamer 내부 thread join 에서 unbounded block 하던 결함 fix. cam 661 의 42분 cam_loss 의 root cause 였음 (backup log 10:09:33 ResetSourceOnly[661] 진입 후 영원히 return 안 함). `gst_element_get_state` timeout 시 unref 건너뛰고 의도된 leak. **5/26 22:00 ~ 5/27 09:06 누적 11.3h 후속 모니터 wd=1 (boot only) / cam_loss=0** (pre-fix 50min wd=6/cam_loss 영구). 단 fix path 미발화 — 다음 자연 stuck 시 실효성 동적 검증 가능.

이 변경들 모두 5/27 audit 으로 검증 — 자체 코드 회귀 0건 (cppcheck 59 동일, clang-tidy 0, ASan/TSan 자체 코드 leak/race 0). 산출물: `master_logs/v0.1.18/audit_20260527_091456/`.

자세한 내용:
- [.DOCS/REVIEW3/SUMMARY.md](../.DOCS/REVIEW3/SUMMARY.md) (3차 review baseline, 레거시)
- [../.DOCS/AUDIT_REPORT_20260519.md](../.DOCS/AUDIT_REPORT_20260519.md) (v0.1.0 audit baseline, legacy)
- [../.DOCS/STUCK_ANALYSIS_cam659_20260520.md](../.DOCS/STUCK_ANALYSIS_cam659_20260520.md) (GstRtsp stale 추적, legacy)
- [../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) (v2.0.0 Search engine 도입 가이드)
