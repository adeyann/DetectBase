# I2 — RKNN API Headers (외부)

## §1. Why

Rockchip NPU(RK3588) 추론 런타임의 C API. DetectBase 가 dlopen 하지 않고 정적 링크하여 사용하는 외부 헤더. F3 의 NPU 추론 엔진이 의존.

제거 시 잃는 것: NPU 추론 불가 → 프로젝트 자체 동작 불가.

---

## §2. Roster

### Primary (I2)

| 파일 | 비고 |
|---|---|
| [rknn_api.h](../../code/Engine/NPU/librknn_api/include/rknn_api.h) | 720 라인. RKNN 추론 API |
| [rknn_matmul_api.h](../../code/Engine/NPU/librknn_api/include/rknn_matmul_api.h) | 260 라인. matmul 전용 (DetectBase 미사용) |

### 동봉 binary (참고)

| 파일 | 크기 | 비고 |
|---|---|---|
| [aarch64/librknn_api.so](../../code/Engine/NPU/librknn_api/aarch64/) | 5.2MB | legacy alias |
| [aarch64/librknnrt.so](../../code/Engine/NPU/librknn_api/aarch64/) | 5.2MB | runtime — Odroid host 의 v1.5.2 라이브러리가 컨테이너에 마운트되어 실제 사용 (Z-2 검증 완료) |

### Also-touches

I2 는 외부 헤더이므로 Primary 는 I2 자체. 사용처는 F3 의 RKNN 엔진 클래스.

---

## §3. How — DetectBase 가 실제 사용하는 API 표면

[YoloV5_Torch_Onnx_RKNN_NPU.cpp](../../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp) 의 grep 결과 기준 (전수 확인).

### 사용 함수 (8개)

| API | 위치 | 역할 |
|---|---|---|
| `rknn_init` | 366 | model 데이터(byte buffer) → context 생성 |
| `rknn_destroy` | (Close 경로) | context 해제 |
| `rknn_query` | 374, 381, 431, 444 | SDK_VERSION / IN_OUT_NUM / INPUT_ATTR / OUTPUT_ATTR |
| `rknn_inputs_set` | 589 | 입력 텐서 데이터 설정 |
| `rknn_run` | 597 | 동기 추론 실행 |
| `rknn_outputs_get` | 600 | 출력 텐서 획득 |
| `rknn_outputs_release` | 647 | 출력 버퍼 해제 |

### 사용 query_cmd

- `RKNN_QUERY_SDK_VERSION` — 런타임 버전 검증
- `RKNN_QUERY_IN_OUT_NUM` — 입출력 텐서 개수
- `RKNN_QUERY_INPUT_ATTR` — 입력 텐서 attr (shape, type 등)
- `RKNN_QUERY_OUTPUT_ATTR` — 출력 텐서 attr

### 미사용 API (확인됨)

- `rknn_set_core_mask` — 멀티 NPU core 분산 (DetectBase 단일 core)
- `rknn_dup_context` — context 복제
- `rknn_create_mem / rknn_destroy_mem / rknn_set_io_mem` — DMA-buf 등 manual memory 관리
- `rknn_matmul_*` — matmul 전용 API (rknn_matmul_api.h 전체)
- `rknn_wait` — 비동기 추론 대기 (DetectBase 는 rknn_run 동기 호출)

### 호출 패턴

```
[Init 1회]
  rknn_init(model_buffer)
  rknn_query(SDK_VERSION) → 버전 로그
  rknn_query(IN_OUT_NUM) → io_num.n_input/n_output
  rknn_query(INPUT_ATTR ×n_input)  → 입력 텐서 명세
  rknn_query(OUTPUT_ATTR ×n_output) → 출력 텐서 명세

[Inference per frame]
  rknn_inputs_set(ctx, n_input, inputs[])
  rknn_run(ctx, nullptr)              ← 동기 차단
  rknn_outputs_get(ctx, n_output, outputs, nullptr)
  ... 후처리 (YOLOv5 디코드, NMS) ...
  rknn_outputs_release(ctx, n_output, outputs)

[Close 1회]
  rknn_destroy(ctx)
```

표면이 매우 좁고 표준적 — 멀티코어/DMA-buf/비동기 등 고급 기능 미사용. ABI 호환성 위험 낮음.

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `rknn_context ctx` | YoloV5_Torch_Onnx_RKNN_NPU 의 멤버 `rknn_ctx_` | 엔진 객체에 종속. Close 에서 rknn_destroy |
| `rknn_input[]` 입력 버퍼 | 엔진 객체 (사전 할당된 멤버) | 엔진 lifecycle |
| `rknn_output[]` 출력 버퍼 | 엔진 객체 (사전 할당) — 단, 내부 buffer 는 rknn_outputs_get 이 채우고 rknn_outputs_release 가 해제 | 매 추론마다 get/release pair |
| model byte buffer | 엔진 객체 멤버 `engine_data_` | rknn_init 이후에는 RKNN 런타임이 자체 복사하므로 lifetime 분리 가능. F3 에서 정확한 보유 기간 검증 필요 |

---

## §5. Concurrency

- DetectBase 는 카메라당 단일 RKNN context 를 생성하고, 단일 inference thread 에서만 호출 (F3 검증 항목).
- RKNN 런타임 자체의 thread-safety 는 벤더 문서에 의존. 일반적으로 ctx 단위 직렬 호출이 안전.
- 멀티 카메라는 별도 ctx (별도 엔진 인스턴스)로 분리. 카메라당 1 thread 패턴.

---

## §6. Findings

### F-I2-01 — `rknn_run` 동기 호출 (블로킹) — 단일 thread 의 frame rate 결정 요인
- **등급**: NOTE
- **위치**: [YoloV5_Torch_Onnx_RKNN_NPU.cpp:597](../../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.cpp#L597)
- **내용**: 비동기 `rknn_wait` 사용 시 IO 와 inference overlap 가능 (이론적). 현재 동기 호출이 단순하지만 NPU idle 시간이 있을 수 있음.
- **현 영향**: DFPS 13.x 측정값. IO 가 비동기(P54-DiskGuard L2-Emergency 등)로 빠진 후의 수치이므로 NPU 자체 한계가 더 큰 영향.
- **권장**: deferred. 성능 향상 필요 시 추후 검토.

### F-I2-02 — 동봉 .so 와 호스트 마운트 .so 의 일관성 의존
- **등급**: NOTE (이미 README 에 명시)
- **내용**: `aarch64/librknnrt.so` (동봉 5.2MB) vs Odroid host 의 v1.5.2 (mount). 컨테이너 내부에서 어느 쪽이 dlopen 되는지에 따라 동작 차이 가능.
- **현 영향**: docker-compose 가 host 의 /usr/lib/librknnrt.so 를 mount 하여 호스트 NPU 드라이버와 일치한 라이브러리 사용. README §"NPU" 와 1차 리뷰에서 검증 완료.
- **권장**: 변경 없음. 단, librknnrt.so 만 사용하고 librknn_api.so 는 미사용이면 빌드 트리에서 정리 가능 (deferred).

### F-I2-03 — RKNN runtime 버전 미스매치 시 std::bad_alloc 사례 기록 (CLAUDE.md Known Issues)
- **등급**: NOTE
- **위치**: README + CLAUDE.md "Header-library version mismatch causes std::bad_alloc"
- **내용**: 헤더와 .so 버전 불일치 시 잠재적 crash. 현재 v1.5.2 + 동봉 헤더(720+260 라인) 조합으로 검증됨.
- **권장**: 변경 없음. RKNN 업그레이드 시 헤더 동시 갱신 필수 — Z-2 git tag 락과 같은 외부 의존 관리 항목.

---

## §7. Open Questions

없음. 외부 헤더이고 표면이 좁아 결정 사항 없음.

---

## §8. Self-Check

- [x] Primary 파일 인벤토리 — 헤더 2개 + .so 2개. 헤더는 외부 벤더 dump 이므로 라인 단위 검토 안 함. 사용 표면만 정리
- [x] §3 호출 시퀀스 — 사용 함수 8개 모두 grep 으로 실제 사용 위치(file:line) 표기
- [x] 미사용 API 명시 — set_core_mask / matmul / dup_context 등 grep 으로 미사용 확인
- [x] §4 소유권 — ctx 멤버 보유, output buffer 의 get/release pair 명시. model buffer 는 검증 필요로 표기
- [x] §5 동시성 — ctx 당 단일 thread 패턴 (F3 에서 검증 필요로 표기)
- [x] §6 Finding 모두 등급 + 출처
- [x] 추측은 "검증 필요" 명시 (model buffer lifetime, ctx-per-thread 패턴)

**검증 결과**: PASS

**보강 필요 항목**:
- F3 분석 시: model buffer 의 정확한 보유 기간 / ctx-per-thread 강제 여부 / inference thread 의 reentrancy 검증
