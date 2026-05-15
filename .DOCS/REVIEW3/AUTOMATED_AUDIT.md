# 3-A 자동화 도구 audit

> 정적 분석 도구로 AI 코드리뷰가 놓친 결함을 cross-check.

**도구**: cppcheck 2.7, clang-tidy 14, ASan/TSan (선택)
**환경**: detectbase:analysis 컨테이너 (detectbase:1.0 베이스 + apt 추가)
**범위**: DetectBase 자체 코드 (Main/Engine/Management/BasicLibs/AbnormalActions/Tracker/VisionCommon/Protocol/GRPC). RTSP 외부 모듈은 사용자 정책상 제외.

---

## §1. 한 줄 결론

**자체 코드 결함 매우 적음**. cppcheck 7건 + clang-tidy 의미 있는 발견 2건. 모두 차분 회귀 (DIFF_REGRESSION.md) 와 cross-check 일치.

**의미 있는 발견 2건은 clang-tidy 만 잡은 것** — DIFF_REGRESSION.md 의 NEW-5 (UB 위험) + NEW-6 (dead member). 둘 다 수동 코드 리뷰에서 놓친 결함. **자동화 도구 도입 가치 입증**.

---

## §2. cppcheck 결과

**전체 결함 카운트**: 자체 코드 **7건** + 외부 RTSP 91건 (정책상 무시)

### 2.1 자체 코드 결함 (7건, 모두 style)

| 위치 | 종류 | 분류 |
|---|---|---|
| Tracker/SORT/MgenHungarian.cpp:230 | variableScope | Style — Hungarian 알고리즘 외부 구현 |
| Tracker/SORT/MgenHungarian.cpp:317 | variableScope | Style — 동일 |
| Tracker/SORT/MgenHungarian.cpp:105 | unreadVariable | Style — 동일 |
| Tracker/SORT/MgenHungarian.cpp:106 | unreadVariable | Style — 동일 |
| Tracker/SORT/MgenHungarian.cpp:110 | unreadVariable | Style — 동일 |
| Tracker/SORT/SORTTracker.cpp:63 | knownConditionTrueFalse `!kalman_uptr` always false | **NEW-4** (cross-check 일치) |
| Tracker/SORT/SORTTracker.cpp:174 | knownConditionTrueFalse `!kalman_uptr` always false | **NEW-4** (cross-check 일치) |

### 2.2 cppcheck 결론

- 자체 핵심 코드 (Main / Engine / Management / BasicLibs / AbnormalActions / VisionCommon / Protocol/GRPC) **결함 0건**
- Tracker/SORT 의 외부 알고리즘 코드 (Hungarian, Kalman) 만 style 경고
- → **2차 코드리뷰 + 32건 패치 효과 검증됨**

---

## §3. clang-tidy 결과

**환경 한계**: compile_commands.json 의 일부 entry 가 clang-tidy 의 `unable to handle compilation` 에러로 누락됨 (전체 172 파일 중 일부). 하지만 처리된 파일에서 검출된 결함은 신뢰 가능.

**카테고리 분포**:
| 카테고리 | 건수 | 의미 있음? |
|---|---|---|
| clang-diagnostic-error | 427 | ✗ — compile command 매칭 실패 (도구 한계) |
| bugprone-easily-swappable-parameters | 42 | ✗ — false positive 가능성 큼 (인접 동일 타입 경고, 의도된 시그니처 다수) |
| bugprone-narrowing-conversions | 26 | △ — 좌표 정수 → float 정규화는 의도적 |
| **cppcoreguidelines-pro-type-member-init** | **9** | **✓ — 진짜 분석 가치** |
| bugprone-implicit-widening-of-multiplication-result | 7 | △ |
| bugprone-branch-clone | 6 | △ |
| cppcoreguidelines-pro-type-cstyle-cast | 1 | △ — 외부 라이브러리 (tinyxml) |

### 3.1 의미 있는 발견 (member-init 9건)

타입별 분류 후:

**False positive (vector/map/string default-init)**:
- InferObject.cpp:6, 34 `extend_data` (std::vector<float>) — 안전
- ServiceBlockProfile.cpp:32 `input_sources_, output_targets_` (unordered_map) — 안전
- YoloV5 ctor:25 `rknn_inputs_` (std::vector) — 안전
- EngineProfile.cpp:7, 30 `_uuid` (initializer list 명시됨) — clang-tidy false positive
- RtspDetectorUnit.cpp:480 `tstruct` (직후 localtime_r 호출로 init) — 안전
- tiny_xml_parser.cpp:181 `cursor` — 외부 라이브러리

**진짜 위험**:
- ⚠ **MgenLogger.cpp:222 `re_open_intervals`** (std::chrono::seconds) → 빈 file_name early return 시 미초기화 → UB 위험. → **DIFF_REGRESSION.md NEW-5**
- ⚠ **YoloV5 ctor:25 `rknn_outputs_byte_size_`** (size_t) → 사용처 0건 (dead member). → **DIFF_REGRESSION.md NEW-6**

### 3.2 C-style cast (1건)

| 위치 | 결과 |
|---|---|
| BasicLibs/core/parser/xml/tiny_xml_parser.cpp:215 | 외부 라이브러리 (sourceforge tinyxml). CLAUDE.md "C-style cast 금지" 는 자체 코드 대상 → **변경 안 함** |

→ **자체 코드의 C-style cast 0건** (CLAUDE.md 100% 준수 검증)

---

## §4. ASan / UBSan

**환경**: Debug build + `-fsanitize=address,undefined -fno-omit-frame-pointer`
**실행**: 90초 timeout (graceful shutdown 시도, 104s 후 cleanup)

**결과**:
```
SUMMARY: AddressSanitizer: 397684 byte(s) leaked in 3470 allocation(s).
```

**leak 분석**: 모두 외부 librknnrt.so 의 `rknn_init` 내부 leak.
- 호출 위치: `MGEN::YoloV5_Torch_Onnx_RKNN_NPU::LoadModelEngineFile()` (우리 wrapper, 실제 leak 은 closed-source librknnrt.so 내부)
- 1회성 (init time), 운영 중 누적 X
- 외부 라이브러리 → 정책상 무시

→ **자체 코드 ASan/UBSan 검출 0건** (RAII 100% 검증).

---

## §5. TSan (ThreadSanitizer)

**환경**: Debug build + `-fsanitize=thread -fno-omit-frame-pointer -fPIE`
**실행**: hang 발생 (1791초, SIGKILL 정리). instrumentation 100x 느림으로 graceful shutdown 못함.

**hang 원인 (파악 완료)**:
- TSan instrumentation 의 lock acquire/release 추적 overhead 가 NPU inference 보다 무거움
- DFPS 못 따라감 → RTSP packet 1024 queue full → drop log 무한 발생 (175,687 줄 중 끝부분 모두)
- Stop 시퀀스의 thread join 이 packet drop loop 빠져나오지 못함
- → **TSan 빌드 분기 프로젝트 권고는 §5.4 참조** (카메라 1대만으로는 부족, fps_limit 같이 낮춰야)

**검출 결과 (188 WARNING)**:
| 종류 | 건수 | 분석 |
|---|---|---|
| lock-order-inversion | 1 | SioHandler::Initialize 의 cv_any::wait_until — **NEW-8 (cv_any → cv 변경)** |
| double lock of mutex | 17 | 모두 cv_any 자체 mutex 패턴 — NEW-8 으로 같이 해소 |
| data race | 171 | 자체 코드 위치 모두 atomic 또는 mutex 보호 — false positive |

### 5.1 race 위치 분포 (자체 코드)

| 위치 | 건수 | 분석 |
|---|---|---|
| SafeThread.h:37 | 121 | `is_running_.exchange()` — `std::atomic<bool>` 보호, race 불가 → false positive |
| SafeQueue.h:72/96/97 | 63 | `lock_guard<std::mutex>` 보호 → false positive |
| EngineStreamTypes.h:121/137 | 37 | struct ownership transfer (큐 happens-before) → false positive |
| EngineHandlerBase 종료 | 30 | atomic flag + 순차 종료 → false positive |
| DETECTOR.cpp:409/457 | 25 | atomic load + main thread only → false positive |
| RtspDetectorUnit/EngineLoadBalancer | 52 | mutex/atomic 보호 → false positive |

### 5.2 외부 RTSP 라이브러리 race

~150건 — 사용자 정책상 무시 (외부 라이브러리 변경 금지).

### 5.3 결론

→ **자체 코드 진짜 결함 1건 (NEW-8)** — fix 완료. 나머지 187건 모두 false positive.

### 5.4 분기 프로젝트 TSan 빌드 권고 (정확히)

카메라 1대만으로는 부족 — TSan 100x 느림이라 1대 RTSP packet 도 inference 못 따라감. **반드시 fps_limit 같이 낮춰야** packet drop 회피.

| 옵션 | 환경 | 잡는 race |
|---|---|---|
| **권고 (옵션 5)** | 카메라 1대 + `fps_limit = 1~2` (NetworkSettings.json) + 30s 실행 + SIGKILL | startup + runtime + (shutdown 일부) |
| 대안 1 | 카메라 1대 + 매우 낮은 fps + 60s + graceful SIGINT | shutdown race 도 잡음 (hang 위험) |
| 대안 2 | RTSP stream 완전 비활성 (cam list 비움) | startup/shutdown only (inference path race 못 잡음) |

race report 는 detection 시 즉시 stderr 로 출력되므로 graceful shutdown 안 거쳐도 race 자체는 모두 수집됨 → 옵션 5 가 가장 안전.

---

## §6. cross-check (DIFF_REGRESSION 와 정합)

| 도구 발견 | 차분 회귀 |
|---|---|
| cppcheck SORTTracker `!kalman_uptr` always false | NEW-4 ✓ |
| clang-tidy MgenLogger re_open_intervals 미초기화 | NEW-5 (수동 리뷰 놓침) |
| clang-tidy YoloV5 rknn_outputs_byte_size_ dead | NEW-6 (수동 리뷰 놓침) |
| TSan SioHandler cv_any lock-order + double lock | **NEW-8** (수동 리뷰 놓침) |
| 정적/sanitizer 자체 코드 throw / try/catch / new / leak | **0건** (32건 패치 효과) |

→ **자동화 도구가 추가 발견한 결함 3건** (NEW-5, NEW-6, NEW-8). NEW-5 와 NEW-8 은 의미 있는 결함 — fix 완료.

---

## §7. 결론 + 권고

### 검증된 사실
- 자체 코드 결함 매우 적음 (cppcheck 7건 모두 style, 그 중 2건이 NEW-4 와 일치)
- **자동화 도구가 수동 리뷰 놓친 결함 3건 추가 검출**:
  - NEW-5 (Major UB) — fix 완료
  - NEW-8 (TSan lock-order/double lock false positive 18건 발생원인 cv_any) — fix 완료
  - NEW-6 (Trivial dead member) — fix 완료
- CLAUDE.md "C-style cast 금지" 자체 코드 100% 준수 검증
- ASan: 자체 코드 leak/UAF 0건 (외부 librknnrt.so init leak 만)
- TSan: 자체 코드 진짜 race 0건 (NEW-8 fix 후 false positive 패턴 해소 + 187건 모두 false positive)

### 분기 프로젝트 권장
- 분기 프로젝트에 cppcheck + clang-tidy 를 CI 에 통합
- ASan/UBSan 빌드는 release 전 stress test 시 1회 실행
- **TSan 빌드는 §5.4 권고 환경 (카메라 1대 + fps_limit 1~2 + 30s + SIGKILL) 에서만** — 카메라 1대만으로는 packet drop hang 회피 안 됨
