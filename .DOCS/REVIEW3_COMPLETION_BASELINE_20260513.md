# 3차 코드리뷰 완료 baseline — 2026-05-09 (Production-ready 확정)

> **레거시 문서 (.DOCS/)**: 이 문서는 2026-05-13 시점에 .DOCS/ 로 이동된 baseline 스냅샷이다.
> 작성 당시의 "다음 세션 첫 명령" 안내는 만료됨. 현재 작업 계획은 [logs/NEXT_SESSION.md](../logs/NEXT_SESSION.md).
>
> **보존 목적**: 3차 코드리뷰 완료 시점의 production-ready baseline 스냅샷 (회귀 비교 / 인수인계용).

---

## 🟢 현재 상태 — production-ready baseline 확정

| 측면 | 결과 |
|---|---|
| **빌드 산출물** | `detectbase:1.0` (prometheus-cpp v1.3.0 + grpc v1.30.2) |
| 분석 이미지 | `detectbase:analysis` (clang-tidy + cppcheck + clang) |
| **마지막 운영 검증** | DFPS 52.4, 4 cam active, ERROR 0, WARN 0, SERVICE START SUCCESS |
| 누적 패치 | 53건 (2차 32건 + 3차 10건 + 트리거 처리 5건 + clang-tidy 100% 추가 fix 5건 + W-14 malloc_trim 1건) |
| 진행 중 검증 | 48h 운영 테스트 (W-14 효과) 2026-05-12 09:44 ~ 2026-05-14 09:44 (logs/test_48h_20260512_094418/) |
| 자체 throw | **0건** (CLAUDE.md 100% 준수) |
| 자체 코드 ASan/UBSan | **0건** |
| 자체 코드 TSan 진짜 race | **0건** (NEW-8 fix 후 187건 모두 false positive) |
| 자체 코드 cppcheck 결함 | **0건** (Tracker SORT 외부 알고리즘 7건만) |
| 자체 코드 C-style cast | **0건** |

---

## 3차 코드리뷰 (A + B + D 통합) 산출물

| 단계 | 도구 | 산출물 |
|---|---|---|
| 3-A 자동화 audit | cppcheck + clang-tidy 100% + ASan/UBSan + TSan | [.DOCS/REVIEW3/AUTOMATED_AUDIT.md](REVIEW3/AUTOMATED_AUDIT.md) |
| 3-B 운영 시뮬레이션 (B-3 하이브리드) | baseline + 시나리오 C/B/A | [.DOCS/REVIEW3/RUNTIME_BEHAVIOR.md](REVIEW3/RUNTIME_BEHAVIOR.md) |
| 3-D 차분 회귀 | 32건 패치 (a)(b)(c) cross-check | [.DOCS/REVIEW3/DIFF_REGRESSION.md](REVIEW3/DIFF_REGRESSION.md) |
| 종합 | 위 셋 통합 | [.DOCS/REVIEW3/SUMMARY.md](REVIEW3/SUMMARY.md) |

---

## 3차 리뷰 신규 발견 8건 + 처리

| ID | 등급 | 내용 | 처리 |
|---|---|---|---|
| NEW-1 | Major | NetworkManager::InitializeGrpcClients catch(...) 누락 — F-F5-12 scope 누락 | ✅ Fix |
| NEW-5 | Major | FileLogger ctor 빈 file_name 시 re_open_intervals 미초기화 → UB (clang-tidy) | ✅ Fix (헤더 default-init) |
| NEW-8 | Major | SioHandler condition_variable_any → condition_variable (TSan 검출) | ✅ Fix |
| NEW-9 | Minor | EngineProfile printf %d → %u 포맷 mismatch (audit cppcheck) | ✅ Fix |
| NEW-10 | Minor | DETECTOR event_binder make_shared nullptr 검사 dead code 2곳 (audit cppcheck) | ✅ Fix |
| NEW-7 | Build | CMakeLists.txt $<CONFIG:Debug> "-O0 -g" 공백 분리 결함 (ASan 빌드 시 발견) | ✅ Fix (옵션 분리) |
| NEW-3 | Trivial | reOpen 빈 file_name 매 호출 재시도 (NEW-5 통합) | ✅ Fix |
| NEW-4 | Trivial | SORTTracker make_unique nullptr 검사 dead code | ✅ Fix |
| NEW-6 | Trivial | YoloV5 rknn_outputs_byte_size_ dead member | ✅ Fix |
| NEW-11 | Trivial | 자체 코드 unused 변수 3건 (audit cppcheck) | ✅ Fix (`_` throwaway) |
| NEW-2 | Minor | rtsp_proxy.cpp dead try/catch | 변경 없음 (외부 라이브러리) |

**총 10건 fix + 1건 정책상 보류**.

---

## 누적 패치 39건 (2차 32 + 3차 7)

### 2차 리뷰 후속 (32건)
- 옵션 B + 자체 throw 11건
- 옵션 C 13건 (메트릭 5+1, graceful, 큐 capacity, 코멘트)
- Root cause fix 1건 (ScheduleSettingData FullArray)
- 옵션 1 NOTE 3건
- 추가 NOTE 4건

자세한 내용: [.DOCS/REVIEW2/SUMMARY.md](REVIEW2/SUMMARY.md)

### 3차 리뷰 후속 (10건)
- NEW-1, NEW-3, NEW-4, NEW-5, NEW-6, NEW-7, NEW-8, NEW-9, NEW-10, NEW-11

자세한 내용: [.DOCS/REVIEW3/SUMMARY.md](REVIEW3/SUMMARY.md)

---

## 핵심 문서 (단일 진입점)

| 정보 | 위치 |
|---|---|
| **3차 리뷰 단일 종합** | [.DOCS/REVIEW3/SUMMARY.md](REVIEW3/SUMMARY.md) |
| 3차 자동화 audit | [.DOCS/REVIEW3/AUTOMATED_AUDIT.md](REVIEW3/AUTOMATED_AUDIT.md) |
| 3차 차분 회귀 | [.DOCS/REVIEW3/DIFF_REGRESSION.md](REVIEW3/DIFF_REGRESSION.md) |
| 3차 운영 시뮬레이션 | [.DOCS/REVIEW3/RUNTIME_BEHAVIOR.md](REVIEW3/RUNTIME_BEHAVIOR.md) |
| 2차 리뷰 단일 종합 | [.DOCS/REVIEW2/SUMMARY.md](REVIEW2/SUMMARY.md) |
| 1차 리뷰 카탈로그 | [.DOCS/CODE_REVIEW_SUMMARY.md](CODE_REVIEW_SUMMARY.md) |
| 사용자용 빌드/실행 | [README.md](../README.md) |
| 코드 모듈 구조 | [code/README.md](../code/README.md) |
| 코딩 컨벤션 | [CLAUDE.md](../CLAUDE.md) |

---

## 잔존 작업 — 카테고리

### 사용자 지시 시 처리 (보류)

- **Debug Virtual Lines 제거** — 시연/검증용 임시 코드. 사용자가 직접 시킬 때만.
  ([code/Main/DETECTOR/src/RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) — `AddDebugVirtualLines_REMOVABLE` 함수 + 호출 2곳)

### 트리거 의존 (자연 발현 또는 분기 시 처리)

| 항목 | 트리거 |
|---|---|
| ~~W-06 RenewAfterReset~~ | ✅ **2026-05-09 fix 완료** (SettingData.cpp 5곳 type check 추가) |
| ~~W-13 server_owner_ raw ptr~~ | ✅ **2026-05-09 fix 완료** (enable_shared_from_this + weak_ptr) |
| F-F5-07 GRPC client closer 실제 path | grpc enable + Master/Slave stress |
| F-F5-08 GRPC peer reconnect | 운영 중 peer down 관찰 |
| F-F5-09 max_message_size | 대용량 image RPC 사용 |
| F-I2-01 rknn_run async | 성능 최적화 |

### 변경 없음 / 사용자 기각

- 외부 ABI 강제: F-F5-02 raw new (gRPC), F-I1-02 new CRtspProxy (RTSP)
- 외부 라이브러리: NEW-2 rtsp_proxy dead try/catch, tinyxml C-style cast
- 의도된 결정: P37 KST, F-F2-07 CameraSettingData 동적 미지원
- 보안 (사용자 기각): V-01/V-02/V-04, M-04a InsecureChannel
- ~~M-08 OPERATIONS.md~~ → ✅ 2026-05-09 작성 완료
- 분기 프로젝트 task: V-03 GoogleTest

### 진행 중 (2026-05-12 ~ 14)

- 48h W-14 효과 검증 테스트 ([test_48h_20260512_094418/](test_48h_20260512_094418/))
- 결과에 따라:
  - RSS plateau → jemalloc 적용 단계
  - RSS 계속 증가 → ASan 으로 leak 검출 → fix → 재테스트

---

## 분기 프로젝트 (Master/Slave) 적용 가이드

### 변경점
- NetworkSettings.json — GRPC enabled true + peers
- GrpcEventServerBase post-processor — Master cross-check / Slave 카운터 송신
- Heartbeat thread 추가
- 도메인 클래스 (classes.yml + AbnormalActions)

### 분기 시 stress test 권장
- Master/Slave 양방향 1시간 부하 → F-F5-01 / W-13 검증
- Slave kill/재시작 → reconnect (F-F5-08)
- 대용량 image RPC → max_message_size (F-F5-09)
- F-F5-07 GRPC closer 실제 path 검증

### 정적 분석 + Sanitizer (한 명령)

```bash
./detectbase.sh audit              # cppcheck + clang-tidy + ASan + UBSan (운영 90초 정지)
./detectbase.sh audit --with-tsan  # 위 + TSan (운영 30초 추가 정지)
```

결과: `logs/audit_<timestamp>/` (cppcheck.log / clangtidy.log / asan_run.log / tsan_run.log + summary.txt)

- 첫 실행 시 분석 이미지 자동 빌드 (~3분), 이후는 캐시 사용
- ASan/TSan 빌드 각 ~5-10분
- **TSan 권고**: 카메라 1대만으로는 부족 (TSan 100x 느림). 깊이 검증 시 NetworkSettings.json 의 fps_limit 1~2 로 임시 변경. race report 는 detection 시 즉시 stderr 출력
- 자세한 내용: [REVIEW3/AUTOMATED_AUDIT.md](REVIEW3/AUTOMATED_AUDIT.md) §5.4

---

## 운영 모니터링

### 로그 / 메트릭 일반

```bash
# 실시간 로그
tail -f logs/DetectBase.log

# 메트릭 (port 9090)
curl -s http://localhost:9090/metrics | grep "^detectbase_"

# ERROR 만
grep '"lvl":"ERROR"' logs/DetectBase.log | tail -30

# 카메라별 추적
grep '"correlation_id":"sys-detector-658"' logs/DetectBase.log | tail -30

# drop / partial_failure 메트릭
curl -s http://localhost:9090/metrics | grep -E "errors_total\{type=\"(emit_drop|io_work_drop|engine_input_q_drop|logger_fail|setting_callback)\"\}|setting_partial_failure"
```

### Debug Virtual Lines 활성 중 (시연 검증)

```bash
docker exec detectbase_service bash -c 'find /frame -name "*.jpg" | wc -l'
grep "IOWorker: failed" logs/DetectBase.log
```

### GRPC 활성 시

```bash
curl -s http://localhost:9090/metrics | grep -E "grpc_(send|recv|enabled|peer)"
```

---

## 핵심 변경 요약 (3차 리뷰 후)

| 변경 | 결과 |
|---|---|
| 자체 throw 11건 → 0건 | CLAUDE.md "C++ 예외 사용 금지" 100% 준수 |
| 메트릭 사각지대 5+1건 모두 보강 | 운영 가시성 ↑ (drop / setting_callback / logger_fail / partial_failure) |
| graceful degradation + UpdateMode root cause fix | 카메라 1개 결함 다른 카메라 영향 X + 모든 schedule 정상 적용 회복 |
| F-F5-07 / F-F5-12 noexcept 정합 | GRPC 외부 throw 시 terminate 차단 |
| NEW-1 InitializeGrpcClients catch(...) | 같은 noexcept 함수 결함 cross-check 발견 + fix |
| NEW-5 FileLogger UB | 빈 file_name 시 미초기화 멤버 비교 UB 차단 |
| NEW-8 SioHandler cv 변경 | TSan false positive 18건 해소 + 효율 ↑ |
| NEW-7 CMakeLists -O 분리 | ASan/UBSan 빌드 시스템 결함 fix |
| 종료 시퀀스 안전성 + 코멘트 강화 | "DO NOT REORDER" 명시, 10초 graceful 검증 |
| 큐 capacity + 메트릭 일관 | 모든 큐 max_size + drop oldest + 메트릭 |

---

## 사용자 환경 정리 (필요 시)

- `/home/claudedev/tmp/rknpu2_v152` — RKNN v1.5.2 검증용
- `/home/claudedev/MAIA`, `/home/claudedev/compose_raid`, `/home/claudedev/compose_search` — 참고 원본
- 백업: `/home/claudedev/detectbase_backup_*.tar.gz`
- 3차 리뷰 raw 검증 데이터: `.deleted_backup/REVIEW3_logs_20260513/sanitizer/_asan_run.log` (3MB), `_tsan_run.log` (21MB), `_clangtidy_v2.log` (80KB), `_cppcheck.log` (20KB) — 분기 프로젝트 cross-check 비교용 (2026-05-13 휴지통 이동)
- 빌드 산출물 (asan_pkg/tsan_pkg/binary, 308MB) 은 `.deleted_backup/REVIEW3_temp_20260509/` 으로 정리됨 (재빌드 가능, [Dockerfile.analysis](../Dockerfile.analysis) + cmake 사용)
