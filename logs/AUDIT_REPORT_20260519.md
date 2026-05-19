# DetectBase audit 보고서 (2026-05-19)

## audit pipeline
`./detectbase.sh audit [--with-tsan]` — 5 종 도구 자동화

| # | 도구 | 종류 | 운영 영향 |
|---|------|------|-----------|
| 1 | cppcheck | static | 0 |
| 2 | clang-tidy | static (cmake configure 후 100% file) | 0 |
| 3 | ASan + UBSan | dynamic (별도 build + smoke) | service stop 필요 (`docker-compose stop` + 자동 재시작) |
| 4 | TSan | dynamic | service stop + 100x slowdown |

결과 위치: `logs/audit_<timestamp>/`

## audit run history

| run | 시각 | duration | 결과 |
|-----|------|----------|------|
| 1 | 2026-05-19 08:24 | ~5 분 | **ASan build 실패** — `detectbase:analysis` image cached (Phase 2 이전 base, GStreamer 없음) |
| 2 | 2026-05-19 08:47 | 23 분 | cppcheck 79 / clang-tidy 166 / ASan 1.2 MB (90s smoke, false positive) |
| 3 | 2026-05-19 09:34 ~ 10:56 | 1h 23분 | cppcheck 79 / clang-tidy 166 / **ASan 1217069 byte (1h runtime 누적 +7616 byte)** — **runtime leak 확정** (GStreamer rtpmanager cleanup) |

## 1. cppcheck 79 건 (style/performance, critical 0)

`grep -cE "^/code/" cppcheck.log` = 79 (52 main + 27 note line).

### 카테고리

| Category | Count | 의미 |
|----------|-------|------|
| `knownConditionTrueFalse` | 9 | v4 instrument 의 `t_*_set` always true (dead code) — ThreadProfiler 마이그레이션 시 자연 정리 |
| `unusedVariable` | 10 | `_` dummy (structured binding) — intentional, false positive |
| `unassignedVariable` | 8 | 동일 패턴 |
| `passedByValue` | 6 | const ref 권장 |
| `useStlAlgorithm` | 5 | for loop → std::any_of/transform |
| `unreadVariable` | 4 | init 값 안 읽음 (loop 안 재 assign) |
| `variableScope` | 3 | scope 줄임 |
| `unusedStructMember` | 2 | FrameDiskBytes::used/capacity |
| `shadowVariable` | 2 | input_data shadow |
| `constParameter` / `constVariable` | 3 | const 선언 |
| **합** | **52** | (note 27 제외) |

### 평가
- production 영향 **0**
- 모두 style / performance hint
- critical / bug / UB **0**

## 2. clang-tidy 166 warning

### 분포

```
69 [bugprone-narrowing-conversions]
44 [performance-unnecessary-value-param]
16 [bugprone-easily-swappable-parameters]
10 [bugprone-implicit-widening-of-multiplication-result]
 5 [performance-move-const-arg]
 4 [performance-no-automatic-move]
 4 [bugprone-branch-clone]
 3 [cppcoreguidelines-pro-type-member-init]
 2 [performance-inefficient-vector-operation]
 2 [bugprone-exception-escape]
 1 [performance-unnecessary-copy-initialization]
 1 [bugprone-use-after-move]
 1 [bugprone-string-literal-with-embedded-nul]
 1 [bugprone-misplaced-widening-cast]
```

### 잠재 14 건 (즉시 fix 대상)

| # | 카테고리 | 수 | 위치 | 평가 | Fix |
|---|---------|----|------|------|-----|
| 1 | **bugprone-use-after-move** | 1 | YoloV5_Torch_Onnx_RKNN_NPU.cpp:517 | UB (POD field 라 동작 OK) | `target_input.size` |
| 2 | **bugprone-exception-escape** | 2 | SettingData.cpp:344, 350 (CameraSettingData ctor) | std::terminate 위험 | try/catch 흡수 (사용자 결정 B) |
| 3 | **bugprone-implicit-widening-of-multiplication-result** | 10 | YoloV5_Torch_Onnx_RKNN_NPU.cpp:200,204,209,210,214,222,571 (7) + MgenHungarian.cpp:175,186,235 (3) | 현 input 크기 OK, 큰 input 대비 방어 | `static_cast<size_t>` 명시 |
| 4 | **bugprone-misplaced-widening-cast** | 1 | RtspDetectorUnit.cpp:875 | overflow, 현 detect_fps_limit_=30 OK | cast 순서 (`2 * static_cast<size_t>(x)`) |

### 안전 152 건 (별도 cleanup PR — refactor/audit-cleanup)

- performance-unnecessary-value-param (44) — `T x` → `const T& x`
- bugprone-narrowing-conversions (69) — 대부분 false positive, fix=static_cast 명시
- bugprone-easily-swappable-parameters (16) — review (보통 false positive)
- performance-move-const-arg (5)
- performance-no-automatic-move (4)
- bugprone-branch-clone (4) — review
- cppcoreguidelines-pro-type-member-init (3) — POD init
- performance-inefficient-vector-operation (2) — reserve
- performance-unnecessary-copy-initialization (1) — const ref
- bugprone-string-literal-with-embedded-nul (1) — review

## 3. ASan/UBSan (audit 2nd, 90s smoke)

### 결과
- 1.2 MB leak / 10412 allocations (5 direct + 174 indirect)

### Direct leak 5건 분석

| # | Size | Location | 평가 |
|---|------|----------|------|
| 1 | 16 KB | `g_malloc` → glib internal (`dl_init`) | **External lib** — glib startup, OS process 종료 시 회수 |
| 2 | 3.4 KB | `YoloV5_Torch_Onnx_RKNN_NPU::AllocateBuffers()` line 458 | **NPU input/output buffer** (3 handler), 운영 lifetime 보존 |
| 3 | 1.1 KB | `YoloV5_Torch_Onnx_RKNN_NPU::AllocateBuffers()` line 445 | 동일 |
| 4 | 240 B | `rknn_init` (librknnrt.so) | **External lib** — librknnrt startup |
| 5 | 17 B | `gst_version_string` (libgstreamer) | **External lib** — gst startup |

### 평가
- **진짜 누수 0** — 모두 startup-time allocation, graceful shutdown 안 한 90s smoke 의 false positive
- v8 12h monitor (RSS plateau 602~657 MB, q_drop 0) 와 일치
- production 영향 0

### 90s smoke 한계
- startup allocation 만 검출. 운영 cycle (EOS reconnect 5분 cycle) 한 번도 도달 안 함
- *시간 지나서 발생하는 runtime leak* 검출 불가

### audit 3rd: long-run ASan (해결 시도)
- Main.cpp 의 SIGUSR1 → `__lsan_do_recoverable_leak_check()` (ASan 빌드 시만 활성, production 영향 0)
- detectbase.sh 의 ASan run timeout 90s → `ASAN_DURATION_MIN` 환경변수 (default 240min)
- interval SIGUSR1 sender: T+5min/15min/30min/60min/120min/240min
- 각 시점 leak 출력 → 누적 차이 = runtime leak 식별

### audit 3rd 새 발견 — Runtime leak 확정

**Direct leak 2 건 (1h 동안 누적, audit 2nd 의 90s smoke 에는 없었음)**:
```
Direct leak of 3808 byte(s) in 119 object(s) × 2 (다른 path)
  call stack:
    glib g_malloc → gstrtpmanager.so → g_object_unref →
    gstreamer set_state → TeardownPipeline (GstRtspReceiver.cpp:185)
    → ResetSourceOnly (GstRtspReceiver.cpp:255)
    → ReconnectWorker (GstRtspClient.cpp:202)
```

- 원인: GStreamer **rtpmanager** (rtspsrc 내부 SSRC list) cleanup 시 g_object_unref leak
- 매 EOS reconnect (5분 cycle) → TeardownPipeline → ~320 byte / reconn leak
- v3 세션 의심 ("매 reconn 5 MB") = 잘못. 실제 ~320 byte / reconn.
- v8 12h monitor 의 RSS plateau (range ±55 MB) noise 안 — 진짜 leak 신호 안 드러남.
- long-term: 24/7 × 1년 = ~340 MB / year (acceptable but real)
- **fix 옵션**:
  - A. 수용 (long-term acceptable, plateau 영향 X)
  - B. in-place reset 강화 (이미 ResetSourceOnly 가 시도, fully unref 안 됨)
  - C. GStreamer 업데이트 (rtpmanager bug fix 버전)
  - D. EOS reconnect 빈도 줄이기

## 4. TSan
- audit `--with-tsan` 옵션 (미실행)
- v0.1.0 머지 후 별도 phase 예정
- dfps 116.5 / 4 cam = 29.13 fps/cam. TSan overhead 10x 가정 = ~3 fps/cam ≥ 사용자 기준 (2 fps/cam) → **TSan 가능**

## 정리

| 영역 | 결과 | 평가 |
|------|------|------|
| cppcheck 79 | style/performance hint only | critical 0 → 별도 cleanup PR |
| clang-tidy 잠재 14 | bug 가능성 | **즉시 fix 적용** (uncommitted) |
| clang-tidy 안전 152 | style/performance | 별도 cleanup PR |
| ASan 1.2 MB | startup allocation (false positive) | 진짜 leak 0, v8 monitor 와 일치 |
| ASan long-run | interval check 인프라 추가 | 진행 중 |
| TSan | dfps 충분 (~3 fps/cam) | v0.1.0 후 별도 phase |
