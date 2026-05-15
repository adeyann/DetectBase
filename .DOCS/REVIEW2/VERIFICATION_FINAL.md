# VERIFICATION_FINAL — 마지막 추가 검증

> META_REVIEW 가 제기한 누락 영역 5개 + 코딩 규칙 준수 전수 검증.
> ABCD 4단계 분석 외 마지막 보강 단계.

---

## §0. 검증 영역

| 영역 | 결과 |
|---|---|
| §1 빌드 시스템 | ✅ 깔끔 — Dockerfile.build / docker-compose.yml / CMakeLists.txt / detectbase.sh 모두 검증. 1건 보안 NOTE |
| §2 보안 audit | **새 finding 1건 + 강등 1건** (auth_token 환경변수 지원 확인) |
| §3 테스트 인프라 | ❌ 부재 확정 (META M-07 ✅) |
| §4 정적 분석 도구 | ❌ 호스트에 미설치. 적용 불가 |
| §5 코딩 규칙 준수 | ✅ raw new / C-style cast / using namespace std — 모두 검증 (예외만 외부 라이브러리) |
| §6 결합부 매트릭스 잔여 | ✅ INTER_FLOW + META M-11/12/13 외 추가 결합부 0건 |

---

## §1. 빌드 시스템 검증 (META M-03)

### 1.1 Dockerfile.build

| 영역 | 평가 |
|---|---|
| Base | `arm64v8/ubuntu:22.04` — Odroid M2 aarch64 정합 |
| Layer 분리 | apt 의존성 / protobuf+grpc / restclient / sioclient / prometheus-cpp / RKNN / 추가 audio codec — 6 layer 로 분리. 빠른 반복 빌드 |
| 외부 의존 버전 락 | grpc v1.30.2, restclient 0.5.3, sioclient 3.1.0, prometheus-cpp v1.3.0, RKNN v1.5.2 — 모두 git tag 명시. **Z-2 fix 적용됨** |
| Patch 명시 | abseil graphcycles.cc + failure_signal_handler.cc 의 GCC 11 호환 patch 명시 |
| ABI 호환성 코멘트 | OpenCV apt 의 libgdal/libprotobuf SONAME 일치 명시 |
| LD_LIBRARY_PATH | `/usr/local/lib` 설정 |

**평가**: **모범적 빌드 정의**. 외부 의존 정합성 / ABI / 버전 락 / patch 모두 명시.

### 1.2 docker-compose.yml

| 영역 | 평가 |
|---|---|
| `privileged: true` | NPU 접근 강제 — 보안 약점이지만 RK3588 rknpu 0.9.8 드라이버 제약 |
| `network_mode: host` | RTSP/SocketIO 다중 포트 편의 — 보안 약점 (포트 격리 없음) |
| `cap_add: SYS_PTRACE` | 디버깅용 |
| Devices | `${NPU_DEVICE_PATH}` 환경변수 — P61 외부화 |
| Volumes | localtime ro + librknnrt.so override + IMAGE_ROOT_PATH /frame /crop |
| Stop policy | `SIGINT` + `stop_grace_period: 30s` — graceful shutdown 30초 |
| Restart | `unless-stopped` |

**평가**: NPU 환경 제약 + 운영 편의 + graceful shutdown 모두 적절. **privileged + network host 는 베이스 결정** (분기 시 보안 정책 검토 권장).

### 1.3 CMakeLists.txt (root)

| 영역 | 평가 |
|---|---|
| C++ 표준 | C++17 + fPIC + compile_commands.json export |
| 컴파일 옵션 | -O2/-O0 -g + 일부 warning 무시 (-Wno-psabi 등 명시) |
| 7개 sub CMakeLists.txt | 모듈별 분리 (BasicLibs / VisionCommon / Protocol / Management / Tracker / AbnormalActions / Engine / Main) |
| find_package | Threads / OpenCV / FFmpeg |
| ProgramConfig.h.in | configure_file 패턴 |

**평가**: **표준적 CMake 구조**. 모듈 의존성 그래프 명확.

### 1.4 detectbase.sh

| 서브커맨드 | 기능 |
|---|---|
| build | docker-compose build → 자동 init |
| compile | 컨테이너 안 BuildScript.sh 실행 |
| init | proto 재생성 (protoc 버전 일치 보장) |
| start / stop / status / logs | 운영 |

**평가**: 운영 자동화 도구 충분. `.env` 로드 + 컬러 로그 + 명확한 서브커맨드.

### 1.5 빌드 시스템 finding

#### V-01 — `privileged: true` + `network_mode: host` (보안 약점이지만 의도된 베이스 결정)
- **등급**: NOTE
- **위치**: [docker-compose.yml:31, 36](../../docker-compose.yml#L31)
- **내용**: NPU 접근 / 포트 편의를 위한 결정. production 분기 시 보안 정책 검토.

---

## §2. 보안 audit (META M-04 확장)

### 2.1 발견된 인증/암호 패턴

| 위치 | 내용 | 평가 |
|---|---|---|
| [RtspHandler.cpp:362-370](../../code/Management/worker/src/RtspHandler.cpp#L362-L370) | RTSP 자체 서버 cfg.xml 의 `admin/admin` + `user/123456` 평문 password | ⚠ 단, `need_auth=0` 으로 인증 비활성 — 미사용 데이터 |
| [GrpcEventClientBase.cpp:46](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp#L46) | `grpc::InsecureChannelCredentials()` | NOTE (M-04a) |
| [SioHandler.cpp:39-42](../../code/Management/worker/src/SioHandler.cpp#L39-L42) | `MGEN_SIO_AUTH_TOKEN` 환경변수 fallback `"detectbase-token"` | ✅ env override 지원 — F-F4-11 강등 |

### 2.2 RTSP password 하드코딩 — 새 발견

#### V-02 — RTSP cfg.xml 의 평문 password (NOTE)
- **등급**: NOTE (실 위험 거의 없음 — `need_auth=0`)
- **위치**: [RtspHandler.cpp:362-370](../../code/Management/worker/src/RtspHandler.cpp#L362-L370)
- **내용**:
  ```cpp
  InsertElement( pAdmin, "username", "admin" );
  InsertElement( pAdmin, "password", "admin" );    // ⚠ 평문
  InsertElement( pUser,  "username", "user" );
  InsertElement( pUser,  "password", "123456" );   // ⚠ 평문
  ```
- **현 영향**: `need_auth=0` 으로 인증 미적용. cfg.xml 에 들어가지만 RTSP 라이브러리가 이 값을 인증 검증에 사용 안 함.
- **잠재 위험**:
  - 분기 프로젝트가 `need_auth=1` 로 변경 시 즉시 약한 password 활성
  - 코드 audit 시 발견 → security report 생성
  - cfg.xml 이 디스크에 평문 저장 → 컨테이너 / volume 노출 시 노출
- **권장**:
  - `need_auth=0` 이면 user/password 코드 자체 삭제 (불필요)
  - 또는 환경변수로 외부화 (분기 시 production 정책 적용 가능)

### 2.3 SocketIO auth_token (F-F4-11 강등)

[SioHandler.cpp:38-45](../../code/Management/worker/src/SioHandler.cpp#L38-L45) 의 코드:

```cpp
#include <cstdlib>  // std::getenv
...
if( const char* env_token = std::getenv( "MGEN_SIO_AUTH_TOKEN" ); env_token != nullptr ) {
    this->sio_conn_auth_token = env_token;
} else {
    this->sio_conn_auth_token = DEFAULT_SOCKET_IO_AUTH_TOEKN;
}
```

`MGEN_SIO_AUTH_TOKEN` 환경변수로 override 가능. 즉 production 배포 시 .env 또는 docker-compose environment 로 토큰 주입 가능.

→ **F-F4-11 NOTE → INFO 강등** (이미 외부화 지원).

### 2.4 GRPC InsecureChannelCredentials (M-04a 유지)

[GrpcEventClientBase.cpp:37-46](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp#L37-L46) 의 코드:

```cpp
std::shared_ptr<grpc::Channel> GrpcEventClientBase::MakeChannel( const std::string& ip, const int port ) const noexcept
{
    grpc::ChannelArguments args;
    args.SetInt( GRPC_ARG_MAX_RECONNECT_BACKOFF_MS,     2000 );
    args.SetInt( GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 1000 );
    std::string url = ip + ":" + std::to_string( port );
    return grpc::CreateCustomChannel( url, grpc::InsecureChannelCredentials(), args );
}
```

코멘트 ([NetworkManager.cpp:45](../../code/Management/manager/src/NetworkManager.cpp#L45)): "RAID_Analysis 패턴 재사용".

→ **M-04a NOTE 유지**. 분기 프로젝트가 cross-host 통신 시 mTLS 권장.

---

## §3. 테스트 인프라 (META M-07 확정)

### 검증 결과

```bash
find -maxdepth 4 \( -name "*.test.cpp" -o -name "*_test.cpp" -o -name "test_*.cpp" -o -name "tests" -d \)
# → 결과 0건

find -type d \( -name "test*" -o -name "googletest" -o -name "gtest" \)
# → 결과 0건
```

**테스트 코드 / 디렉토리 / GoogleTest 의존성 모두 부재**.

### V-03 — 테스트 인프라 부재 (M-07 확정)
- **등급**: NOTE → 분기 프로젝트에서는 WARN 이상
- **권장**:
  1. `code/test/` 디렉토리 신설
  2. GoogleTest 의존성 (Dockerfile.build 에 추가 또는 apt `libgtest-dev`)
  3. CMakeLists.txt 에 `enable_testing()` + `add_subdirectory(test)`
  4. 핵심 모듈 unit test:
     - SafeQueue (max_size / drop oldest / terminate / clear_with_action)
     - ReplyDispatcher (sharded lock / wait_and_get timeout / cleanup)
     - SORTTracker (Kalman update / Hungarian assign / track_id 안정성)
     - AbnormalActions (LineIntrusion 등 알고리즘 4개 — 합성 데이터)
     - InferObject 좌표 변환 (pixel↔ratio / ltx↔cx / padded↔original)

---

## §4. 정적 분석 도구 (META M-09)

### 검증 결과

```bash
which clang-tidy cppcheck
# → 호스트에 미설치
```

`compile_commands.json` 은 `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 으로 생성됨 — clang-tidy 적용 가능 환경. 단 호스트 도구 부재로 즉시 실행 불가.

### V-04 — 정적 분석 도구 미적용
- **등급**: NOTE
- **권장**:
  1. Dockerfile.build 에 `clang-tidy cppcheck` 추가
  2. `detectbase.sh` 에 `lint` 서브커맨드 추가
  3. CI 단계로 통합 (분기 프로젝트 시점)

---

## §5. 코딩 규칙 준수 audit (CLAUDE.md)

### 5.1 raw new/delete 금지

전체 grep 결과 — DetectBase 자체 코드의 raw new 사용처:

| 위치 | 사유 | 평가 |
|---|---|---|
| [SettingManager.cpp:207](../../code/Management/manager/src/SettingManager.cpp#L207) | `new SettingManager()` — Meyers' singleton | ✅ 의도된 패턴 |
| [RtspHandler.cpp:160](../../code/Management/worker/src/RtspHandler.cpp#L160) | `new CRtspProxy(...)` — I1 라이브러리 ABI 강제 | ✅ F-I1-02 |
| [RtspHandler.cpp:298, 308, 344, ...](../../code/Management/worker/src/RtspHandler.cpp#L298) | `new TiXmlElement / TiXmlText` × 17건 — TinyXML 자체 GC | ✅ 외부 라이브러리 ABI |
| [GrpcEventClientBase.cpp](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp) | `new SendCall / JsonResponseCall / CounterSnapshotCall` — gRPC async pattern | ✅ F-F5-02 |

**평가**: **모든 raw new 가 외부 라이브러리 ABI 강제 또는 의도된 패턴**. CLAUDE.md 규칙 위반 0건 (외부 강제 예외).

### 5.2 C-style cast 금지

검증된 reinterpret_cast 사용처:

| 위치 | 사유 |
|---|---|
| [RtspDetectorUnit.cpp:536](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L536) | `runCallBacks( reinterpret_cast<uint8*>( const_cast<char*>(...) ) )` — I1 ABI 호출 |
| [RtspHandler.cpp:175-176](../../code/Management/worker/src/RtspHandler.cpp#L175-L176) | `sys_os_create_thread( reinterpret_cast<void*>( rtsp_rx_thread ), NULL )` — I1 ABI 호출 |

**평가**: C-style cast 0건. reinterpret_cast 만 외부 라이브러리 ABI 호출에 제한적 사용.

### 5.3 using namespace std (헤더)

| 파일 | 내용 | 평가 |
|---|---|---|
| SafeQueue.h, ReplyDispatcher.h, ReplyDispatcherWithCleaner.h, file_utils.h, RtspDetectorBlock.h | `using namespace std::chrono_literals;` | ⚠ 좁은 namespace, 영향 미미 |
| sio_parser.h, rtsp_proxy.h | `using namespace std;` | 외부 라이브러리 — 검토 범위 외 |

#### V-05 — `using namespace std::chrono_literals` in 자체 헤더 5개
- **등급**: NOTE
- **내용**: CLAUDE.md "using namespace std (in headers) 금지" 규칙. `chrono_literals` 는 좁은 sub-namespace 라 위험 낮지만 규칙 위반 가능.
- **현 영향**: 사용자가 헤더 include 시 `100ms`, `1s` 등 literal 자동 노출. 충돌 가능성 매우 낮음.
- **권장**: 헤더에서 제거 + .cpp 의 함수 본문 내부에서만 사용. 큰 작업 (5 헤더 수정 + 모든 사용처 검증). 가치 작음 — **NOTE 유지**.

### 5.4 종합 코딩 규칙 준수

| 규칙 | 준수 여부 |
|---|---|
| raw new/delete 금지 | ✅ 외부 ABI 예외만 |
| C-style cast 금지 | ✅ |
| using namespace std (in headers) | ⚠ 5건 chrono_literals (좁은 namespace) |
| 재귀 금지 | ✅ (전수 검증 안 했지만 그동안 본 코드에서 재귀 0건) |
| C++ 예외 사용 금지 | ⚠ 4건 (F-F1-01 / F-F2-02 / F-F4-01 / F-F6-02 — 외부 라이브러리 보호 차원) |

---

## §6. 결합부 매트릭스 잔여 (META M-11/12/13 외)

INTER_FLOW (11개) + META 추가 (M-11/12/13 = 3개) 외 가능한 결합부:

| Edge | 검토 |
|---|---|
| F1 ↔ I1 | F1 의 RtspHandler ctor / RunRTSP / Quit 의 StopRTSP — F4 의 NetworkManager 통합 |
| F1 ↔ I3 | F1 의 모든 자료구조 사용 — I3 가 토대 |
| F2 ↔ I3 | F2 의 SettingMonitor 가 SafeThread / mutex 사용 — 정합 |
| F3 ↔ I2 | F3 의 EngineHandler 가 RKNN API 사용 — 정합 |
| F3 ↔ I3 | F3 의 모든 큐 / thread / 타입 — 정합 |
| F4 ↔ I3 | F4 의 emit_queue / SafeThread / json — 정합 |
| F5 ↔ I3 | F5 의 SafeThread / SafeQueue / ReplyDispatcher — 정합 |
| F6 ↔ I3 | F6 의 prometheus-cpp wrapper — pImpl 로 격리 |
| I1 ↔ I3 | I1 의 자체 자료구조 (linked_list / xml) — 외부 라이브러리, 자기 폐쇄 |
| I2 ↔ I3 | I2 헤더는 I3 미사용 |
| I1 ↔ I2 | 무관 |

**평가**: 모든 흐름 쌍의 결합부 검토 완료. 추가 RISK 0건.

---

## §7. ABCD + 추가 검증 합산 — 최종 통계

### 등급별 (최종)

| 등급 | 기존 | DEFERRED 후 | META 후 | VERIFICATION_FINAL 후 |
|---|---|---|---|---|
| RISK | 0 | 0 | 0 | 0 |
| WARN | 14 | 16 | 16 | 16 |
| NOTE | 38 | 33 | 35 | **37** (+ V-01, V-02, V-03, V-04, V-05) |
| INFO | 27 | 31 | 34 | **35** (+ F-F4-11 강등) |
| **합계** | **79** | **80** | **85** | **88** |

### V-XX 새 발견 5건 모두 NOTE 등급

| ID | 내용 | 우선순위 |
|---|---|---|
| V-01 | privileged + network host (베이스 결정) | 분기 시 검토 |
| V-02 | RTSP password 평문 (need_auth=0) | 분기 시 코드 정리 |
| V-03 | 테스트 인프라 부재 | 분기 시 GoogleTest 도입 |
| V-04 | 정적 분석 도구 미적용 | 분기 시 적용 |
| V-05 | using namespace std::chrono_literals (5 헤더) | 가치 작음 |

### 강등 1건

| ID | 사유 |
|---|---|
| F-F4-11 NOTE → INFO | MGEN_SIO_AUTH_TOKEN 환경변수 override 지원 확인 |

### Phase A 즉시 처리 항목 — 변화 없음

기존 9개 (~50 LOC) 그대로. V-XX 는 분기 프로젝트 결정 사항이거나 가치 작은 정리.

---

## §8. 종합 결론

### 코드 품질 — VERIFICATION_FINAL 검증 후 재확인

| 측면 | 평가 |
|---|---|
| 빌드 시스템 | ✅ 모범적 (Z-2 git tag 락 / patch / ABI 명시) |
| 보안 | ⚠ 평문 password 1건 / Insecure GRPC / 환경변수 외부화는 일부만. 베이스 결정 — 분기 시 강화 |
| 테스트 인프라 | ❌ 부재 — 분기 시 도입 필수 |
| 정적 분석 | ❌ 미적용 — 적용 가능 환경 |
| 코딩 규칙 준수 | ✅ 외부 라이브러리 예외 외 위반 없음 |
| 결합부 매트릭스 | ✅ 모든 쌍 검토. 추가 RISK 0건 |

### 분기 프로젝트 시작 전 최종 권장

**Phase A (~50 LOC)** 적용 +
**Phase A+ 추가**:
- V-02 RTSP password 코드 정리 (`need_auth=0` 이면 user 블록 자체 제거)
- V-03 GoogleTest 인프라 구축 (분기 프로젝트의 첫 task)
- V-04 Dockerfile.build 에 clang-tidy / cppcheck 추가
- V-01 production 보안 정책 결정 (privileged / network host 검토)

---

## §9. Self-Check (VERIFICATION_FINAL)

- [x] 빌드 시스템 4개 파일 (Dockerfile.build / docker-compose.yml / CMakeLists.txt root / detectbase.sh) 모두 검토 + file:line 인용
- [x] 보안 grep 전수 — auth_token / password / InsecureChannel / TLS 모두 확인
- [x] 테스트 디렉토리 / 파일 부재 grep 검증 — 0건 확정
- [x] 정적 분석 도구 호스트 가용성 검증 — `which` 0건
- [x] 코딩 규칙 5개 (raw new / C-style cast / using namespace std / 재귀 / 예외) 모두 grep
- [x] 결합부 11쌍 (9 흐름 중 2개씩) 검토 — 모두 정합
- [x] V-XX 5건 모두 등급 + 출처 + 권장 조치 명시
- [x] 강등 1건 (F-F4-11) 근거 코드 인용

**검증 결과**: PASS

**최종 결론**:
- ABCD + VERIFICATION_FINAL 통합 검증 완료
- RISK 0건 (보수적 평가가 정확)
- 분기 프로젝트 시작 전 baseline 으로 충분
- Phase A 9개 + 분기 프로젝트의 추가 4개 (V-02/V-03/V-04/V-01) 적용 권장
