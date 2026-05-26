# Multi-Engine Architecture — v2.0.0 도입 가이드

**작성**: 2026-05-26
**목적**: 2.0.0 major bump 에 Search engine (또는 다른 type) 도입 시 참고 문서.
**참고 base**: MAIA 프로젝트 (`/home/claudedev/MAIA/`)

---

## 1. 배경

DetectBase 1.0.x 는 **detection only** — `YOLOv5s` 1 종 NPU 추론. 4 cam 환경에서 NPU 천장 (~140 FPS) 의 83% 사용.

미래 운영 시나리오에서 detection 만으로 부족한 케이스:
- **Search / Attribute Classification**: 사람 객체에서 옷, 색상, 외형 attribute 추출 → re-identification 기반
- **Multi-domain detection**: Fire / Smoke / Person 별도 모델 동시 운영
- **Multi-modal**: RGB + IR 동시 처리

v2.0.0 에서 위 시나리오 일부 도입 검토.

---

## 2. MAIA 의 multi-engine 운영 패턴

### 2.1 Engine 분류 (MAIA)

```
engines/
├── Person/        — Detection (YOLOv5l) — RGB/IR 별, Batch 1/2/4
├── Fire/          — Detection — RGB/IR
├── Smoke/         — Detection — RGB/IR
└── Search/        — Classification (ResNet50) — attribute search
```

`EngineSettings.json` 에 다중 entry 등록, `Enable: true/false` 로 활성화 제어.

### 2.2 Engine type 식별 — `ModelMagicType`

`engine.profile.json`:
```json
{
  "ProfileName": "Attribute_Classification",
  "ModelMajorType": "Classification",
  "ModelMagicType": "SearchEngine",       // ← multi-engine 식별 key
  ...
}
```

vs detection:
```json
{
  "ProfileName": "YoloV5_Airockchip_RKNN",
  "ModelMajorType": "Detection",
  "ModelMagicType": "DetectionEngine",
}
```

`ModelMagicType` 기반으로 routing 분기.

### 2.3 ★ Search 호출 패턴 — **Event-driven (Full per-object 아님)**

`/home/claudedev/MAIA/code/Main/RAID/src/RAID_RtspDetectorUnit.cpp:1594-1700`:

```cpp
// USE Search
if( this->use_search_service_ ) {
    // 개별 이벤트에 대해서 순회 ← Event 발생 시점만
    for( auto& target_event : event_list ) {
        for( auto& detected_obj : target_event.on_event_results ) {
            if( detected_obj.class_id != class_id_person &&
                detected_obj.class_id != class_id_car ) continue;
            // bbox crop → resize → search inference (sync)
            auto infer_result_opt = load_balancer_->Request(
                search_engine.BuildRequest( shared_crop_blob, 0 ) );
            // result → event JSON 의 Analysis 노드에 attach
        }
    }
}
```

핵심 특성:
- **매 frame 호출 X** — event 발생 시점만
- **Event 안 객체 (person/car) 만** — class filter
- **Synchronous (await)** — event handler thread 가 결과 대기
- **NPU 부담 미미** — event 빈도 × 객체 수 (평균 ~2 inference/sec)

이는 "Full per-object every frame" (600 inference/sec) 과 다름 — **MAIA 도 NPU/GPU 부담 회피 위해 event-driven 채택**.

---

## 3. DetectBase 1.0.x 의 잔존 / 누락 분석

### 3.1 잔존 (재사용 가능) ✅

| 영역 | 위치 | 상태 |
|---|---|---|
| `EngineProfileMap` (multi-engine 자료구조) | `BasicLibs/profile/EngineProfile.h:14-` | 그대로 사용 가능 |
| `SearchEngineUUID()` (engine 선택 logic) | `BasicLibs/profile/EngineProfile.cpp:133-196` | MAIA 와 동일, 동작 |
| `ModelMagicType` parse + 비교 | `EngineProfileParser.cpp:121,163`, `EngineProfile.cpp:112,160` | 그대로 |
| `InferEngineID` 기반 routing 인터페이스 | `EngineLoadBalancer.h:183` (`engine_profiles_`) | 자료구조 multi-engine 지원 |
| `EngineHandlerBase`, `EngineBuilder` | `Engine/EngineBase/`, `Engine/NPU/EngineBuilder/` | 다른 type handler 추가 가능 |

### 3.2 제거됨 (재도입 필요) ⚠️

`EngineLoadBalancer.cpp`: MAIA 1163 → DetectBase 339 lines (824 lines 단순화).

제거된 logic:
- `target_engine_id` 기반 dispatch (현재 round-robin handler 만)
- `engine_resource_weights_map_` (weight-based device 분배)
- batch optimization (배치 크기 별 정렬)
- engine 별 device set
- fuzzy search fallback (engine 누락 시 호환 engine 찾기)

평가: 1.0.x 의 simple LoadBalancer 가 2.0.0 base 로 적합 — 필요한 부분만 점진 추가.

### 3.3 새로 작성 필요

- NPU 용 Search engine handler (Classification type)
  - MAIA 의 `Resnet_Keras_Onnx_TRT_GPU` 는 GPU 기반 → NPU (RKNN) 변환 필요
  - 새 `Resnet_*_RKNN_NPU/` 디렉토리, EngineHandlerBase 상속
- Tracker / Event handler 에서 SearchEngine 호출 path
  - `RtspDetectorUnit::EventHandlerThread` 에 search 호출 추가
- Event JSON 의 `Analysis` 노드에 search result attach

---

## 4. NPU 부담 추산 (Odroid M2 NPU only)

### 4.1 Detection only (현재)
- 4 cam × 30 FPS = 120 inference/sec
- NPU 천장 (s 모델) 140 FPS → 86% 사용

### 4.2 Detection + Event-driven Search (MAIA 방식)
- 평균 event 빈도 = ~1 / 5~10s per cam = ~0.2 / sec
- 4 cam × 0.2 × 평균 3 객체 = **~2.4 search inference/sec**
- s detection (17ms) + ResNet50 NPU (추정 30~50ms)
- 추가 NPU 부담 = 2.4 × 40ms = 0.1초/sec = **약 10% 추가 부하**
- **합계 86% + 10% ≈ 96%** — 운영 가능 (peak 시 일시 100% 도달 가능)

### 4.3 Full per-object every frame (실현 불가)
- 4 cam × 30 FPS × 평균 5 객체 = 600 search inference/sec
- NPU 천장 8x+ 초과 → **불가능**
- → 이 패턴은 채택 안 함. **반드시 event-driven**.

### 4.4 6+ cam scale-up 시
- 6 cam × 30 FPS = 180 FPS (detection only) > NPU 천장 140
- → batch>1 도입 필요 (별도 latent bug fix, NEXT_SESSION 기록됨)
- search 도입 시 batch>1 + event-driven 조합

---

## 5. 도입 단계 (제안)

### Phase 1 — Infrastructure 확장 (1주)
1. `EngineLoadBalancer::RequestAsync()` 에 `target_engine_id` 기반 routing 추가
   - 현재 round-robin (engine type 무시) → MAIA 처럼 `SearchEngineUUID(...)` 로 engine 선택 → 해당 handler 의 input_q 분산
2. `engine_handles_` 를 `std::map<EngineType, std::vector<HandleUUID>>` 로 확장
3. handler 등록 시 engine type 별 분류 (DetectionEngine / SearchEngine)

### Phase 2 — Search engine handler (1주)
1. `Engine/NPU/Resnet50_RKNN_NPU/` 디렉토리 생성
   - `EngineHandlerBase` 상속
   - input 224×224×3, output classification vector
2. ResNet50 ONNX → RKNN 변환 (rknn-toolkit2)
3. EngineBuilder 에 Resnet 빌더 추가 (MagicType match)
4. `engine.profile.json` 의 Search profile 작성

### Phase 3 — Detection → Search workflow (3일)
1. `RtspDetectorUnit::EventHandlerThread` (또는 EvtThread) 에 search 호출 추가
   - MAIA `RAID_RtspDetectorUnit.cpp:1594-1700` 패턴
   - event 발생 시 객체 별 crop → resize → `Request()` sync call
2. event JSON 에 search result attach
3. SearchClassConverter (MAIA 참고)

### Phase 4 — 검증 (1주, major bump 요건)
1. audit 5종 (cppcheck / clang-tidy / ASan / UBSan / TSan) 모두 통과
2. **10h+ aging 모니터링** — memory / FD / Thread leak 추적
3. **10h+ stress 모니터링** — max cam / event burst 시 NPU 천장 도달 측정
4. monitor.sh 에 search-specific metric 추가 (search_inference_total per cam_id)
5. 사용자 명시 허가 (master merge gate, CLAUDE.md §Master merge gate)

### Phase 5 — Release (1일)
1. cmake VERSION 1.0.x → 2.0.0
2. develop → master merge (PR + 사용자 명시 허가)
3. git tag `v2.0.0`

**총 추산: ~3-4주**

---

## 6. 모델 변환 — ResNet50 NPU 운영

### 6.1 현 MAIA 모델
- `asan_latest_search_resnet50_trt8.engine` — TensorRT 8 (GPU 전용)
- NPU 호환 안 됨

### 6.2 NPU 변환 절차
1. 원본 ONNX 또는 Keras 모델 확보
2. `rknn-toolkit2` 로 RKNN 변환:
   ```bash
   from rknn.api import RKNN
   rknn = RKNN()
   rknn.config(target_platform='rk3588', ...)
   rknn.load_onnx('resnet50.onnx')
   rknn.build(do_quantization=True, dataset='./calib_dataset')  # INT8
   rknn.export_rknn('resnet50_rk3588.rknn')
   ```
3. `engines/Search/resnet50_rk3588.rknn` 배치
4. `engine.profile.json` 의 `EngineFilePath` 갱신

### 6.3 NPU 처리 시간 예상
- ResNet50 (community 측정값, RK3588):
  - 1-core: ~25~40ms (INT8 224×224)
  - 3-core combined: ~10~15ms
- search inference 약 30ms 가정 시 천장 ~33/sec

---

## 7. 변경 영역 요약 (file-level)

### 7.1 수정 필요
- `code/Management/manager/include/EngineLoadBalancer.h` — multi-engine API 추가
- `code/Management/manager/src/EngineLoadBalancer.cpp` — routing logic 확장
- `code/Main/DETECTOR/src/RtspDetectorUnit.cpp` — search 호출 path (EvtThread or new thread)
- `code/Main/DETECTOR/include/RtspDetectorUnit.h` — search_engine_ member
- `Dockerfile.build` — search 모델 별도 download / mount (선택)

### 7.2 신규 작성
- `code/Engine/NPU/Resnet_Torch_Onnx_RKNN_NPU/` — Search engine handler
- `engines/Search/` — RKNN 모델 + profile.json + classes.yaml
- `settings/EngineSettings.json` — Search engine entry 추가
- `code/Main/DETECTOR/include/SearchClassConverter.h` — MAIA `RAID_SearchClassConverter` 참고

### 7.3 그대로 사용
- `code/BasicLibs/profile/EngineProfile.{h,cpp}` ✅
- `code/BasicLibs/profile/EngineProfileParser.{h,cpp}` ✅
- `code/Engine/EngineBase/include/EngineHandlerBase.h` ✅

---

## 8. NEXT_SESSION 의 batch>1 fix 와의 관계

NEXT_SESSION 의 latent issue:
> NPU batch_size — code 가 batch=1 hard-assumed (line 412, 505 의 버그)

multi-engine 도입 시 영향:
- **Search engine 도 batch=1 으로 시작 권장** (event-driven 이라 batch>1 의 throughput 이득 미미)
- Detection engine 의 batch>1 fix 는 별개 (6+ cam 강화 시 검토)
- 둘 다 같은 codebase 의 latent bug 라 묶어서 fix 가능

---

## 9. 참고 코드 (MAIA)

| 영역 | MAIA path | 비고 |
|---|---|---|
| LoadBalancer multi-engine routing | `code/Management/manager/src/EngineLoadBalancer.cpp:361-500` | RequestAsync 의 target_engine_id 처리 |
| SearchEngineUUID lookup | `code/BasicLibs/profile/EngineProfile.cpp:133-196` | DetectBase 에도 그대로 잔존 |
| Search inference call site | `code/Main/RAID/src/RAID_RtspDetectorUnit.cpp:1594-1700` | event-driven crop → search |
| SearchClassConverter | `code/Main/RAID/src/RAID_SearchClassConverter.cpp` | classification result → event JSON |
| Engine type initialization | `code/Main/RAID/src/RAID_RtspDetectorUnit.cpp:881-891` | search_engine.Init(...) |

---

## 10. master merge 요건 (CLAUDE.md §Master merge gate)

major bump (1.x → 2.0.0) 의 요건:

| 항목 | 기준 |
|---|---|
| audit 5종 | 모두 통과 |
| **10h+ aging 모니터링** | memory / FD / Thread leak 0 |
| **10h+ stress 모니터링** | max cam (8~12) / event burst 안정성, DFPS dip 분포, wd 빈도 |
| 사용자 명시 허가 | 필수 |

monitor.sh 의 JSONL 출력으로 검증 — 추가 metric:
- `search_inference_total{cam_id}` — search inference 누적
- `search_inference_latency_us` — search 호출 average duration
- `event_to_search_gap_ms` — event 발생 후 search 완료까지

---

## 11. 결정 사항 (2.0.0 시점에 확정)

- search 모델 종류 (ResNet50 / MobileNet / 경량 alternative)
- event 어떤 type 에 search 적용 (LineIntrusion / AreaIntrusion / 모두?)
- multi-engine 확장성 (Fire / Smoke 동시 검토 필요?)
- batch>1 + search 조합 ROI

---

## 12. 갱신 이력

- 2026-05-26: 초기 작성 — MAIA 분석 + DetectBase 잔존 코드 파악 + 단계별 도입 계획
