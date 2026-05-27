# NPU 모델 별 inference / dfps 예측 reference

**환경**: Odroid M2 (RK3588 NPU, librknnrt 1.5.2 / Driver 0.9.8), Camera 29.13 FPS (실측), 3-core multi-handler

## YOLOv5 INT8 @ 640×640 — RKNN inference 시간

| 모델 | params | GFLOPs | 1 core inference | 1 core FPS | 3 core 천장 (scheduling -10%) | COCO mAP |
|------|--------|--------|------------------|------------|-------------------------------|----------|
| **YOLOv5s** (현재) | 7.2M | 16.5 | **17ms** (실측) | 52 | **140 FPS** | 37.4% |
| YOLOv5m | 21.2M | 49.0 | ~35-45ms (추정) | 22-28 | **~67 FPS** | 45.4% (+8.0%) |
| YOLOv5l | 46.5M | 109.1 | ~70-90ms (추정) | 11-14 | **~34 FPS** | 49.0% (+11.6%) |
| YOLOv5x | 86.7M | 205.7 | ~130-160ms (추정) | 6-8 | **~20 FPS** | 50.7% (+13.3%) |

**비고**:
- GFLOPs 와 NPU 시간 정비례 안 함 (memory bandwidth, BPU utilization 차이)
- m/l/x 의 시간은 community 측정값 기반 추정. **실측 권장**.
- INT8 quantization 후 측정. FP16 는 ~2배 느림.

## Cam 수 별 dfps 예측

### 4 cam 환경 (camera 천장 = 29.13 × 4 = 116.5 FPS)

| 모델 | NPU 천장 | 실제 dfps | drop vs camera |
|------|---------|-----------|----------------|
| s (현재) | 140 FPS | **116.5** (camera 천장) | 0% |
| m | ~67 | **~67** (NPU 천장) | -42% |
| l | ~34 | **~34** | -71% |
| x | ~20 | **~20** | -83% |

### 6 cam 환경 (camera 천장 = 29.13 × 6 = 174.8 FPS)

| 모델 | NPU 천장 | 실제 dfps | drop vs camera |
|------|---------|-----------|----------------|
| s | 140 | **~140** (NPU 천장) | -20% |
| m | ~67 | **~67** | -62% |
| l | ~34 | **~34** | -81% |

### 8 cam 환경 (camera 천장 = 29.13 × 8 = 233 FPS)

| 모델 | NPU 천장 | 실제 dfps |
|------|---------|-----------|
| s | 140 | ~140 (NPU bottleneck) |
| m | ~67 | ~67 |

## 다른 stage 천장 (모델 무관)

| Component | 천장 | bottleneck 진입 시점 |
|-----------|------|---------------------|
| Camera FPS (1대) | 29.13 FPS (실측) | — |
| Decoder (avdec_h264 software, CPU 8 core) | ~240 FPS | 16+ cam (s 모델) |
| Pre (sws_scale SWS_AREA) | ~91 FPS/cam (11ms) | 모든 cam (NPU 보다 빠름) |
| Pre (RGA hardware, deferred B1) | ~500 FPS/cam (2ms) | — |
| RspThread cycle (B3 후) | ~25ms | NPU response 시간에 종속 |

## 의사결정 가이드

1. **realtime 우선** (실시간 알림, 4 cam) → **s** (현재) — 116.5 FPS = camera 천장 100% 활용
2. **accuracy 우선** (4 cam, mAP +8%) → **m** — 67 FPS = cam 당 16.7 FPS (운영 가능)
3. **고정확도 + 적은 cam** (2 cam, mAP +11.6%) → **l** — 34 FPS = cam 당 17 FPS
4. **최고 정확도** (1~2 cam, mAP +13.3%) → **x** — 20 FPS = cam 당 10~20 FPS (한계)

**RGA hardware scaler (B1 deferred)** 적용 시 pre 11ms → ~2ms = cam thread cycle 단축. NPU 자체 시간은 안 줄어듦. **모델 m/l/x 도입 시 함께 검토**.

## 실측 검증 방법

1. `settings/EngineSettings.json` 의 `EngineFilePath` 를 `/DetectBase/engines/yolov5m.rknn` 같이 변경 (해당 .rknn 파일 필요)
2. service restart (`docker restart detectbase_service`)
3. INF-thread log 의 `req=N` (NPU 응답 ms) 확인 — 단일 inference 시간
4. RSP-thread log 의 `resp=N` (NPU 왕복 us) 확인 — 4 cam combined
5. `detectbase_dfps_total` Prometheus metric 으로 실제 dfps

## 측정 baseline (실측, 2026-05-18)

- 모델: YOLOv5s INT8 @ 640×640
- cam: 4
- camera 실측 FPS: 29.13 (sample 16k+ frame_cb interval 평균 34.3ms)
- NPU inference 단일: rknn_run 17ms + outputs_get 2ms = 19ms
- B2 + B3 적용 후 dfps: 116.5 (camera 천장 100% 도달)
- inflight_q max: 0~6 (cap 60 의 10%)
- event_q drop: 0 (B3 + DEBUG virtual lines 100% event 환경에서도)

## 갱신 이력

- 2026-05-18: 초기 작성, YOLOv5s 실측 + m/l/x 추정
