# 서드파티 라이브러리 버전 감사 (2026-05-28)

DetectBase 가 의존하는 third-party 라이브러리의 현재 vs 최신 stable 비교.
범위: **Category ② "상대방 (MAIA 서버 / cam) 영향 없는 라이브러리"** — independent bump 가능한 것들.
(Category ① "wire-protocol coupled" = protobuf / gRPC / sioclient — MAIA 와 동기 협의 필요, 본 문서 범위 밖.)

## 베이스 환경
- Docker base: `arm64v8/ubuntu:22.04` (Ubuntu 22.04.5 LTS Jammy Jellyfish, container 가동 확인 5/28)
- Dockerfile.build 소스 빌드 / apt 설치 / host mount 의 3가지 공급 방식

## 비교표

| 라이브러리 | 현재 | 빌드일 | 최신 stable | 출시일 | gap | 공급 | bump 부담 |
|---|---|---|---|---|---|---|---|
| **GStreamer** | 1.20.3 (apt `libgstreamer1.0-0` 1.20.3-0ubuntu1.1) | 2022-06 | **1.28.3** | 2026-05-11 | 4년 / 8 minor | apt | 중 — 22.04 apt 최대 1.20. 1.28 = 소스 or 24.04 (1.24) or PPA |
| **libcurl** | 7.81.0 (apt `libcurl4` 7.81.0-1ubuntu1.24) | 2022-01 / 패치 2024 | **8.20.0** | 2026-04-29 | 4.5년 / major 1→2 | apt | 중 — 8.x = 24.04 (8.5) or 소스 |
| **OpenSSL** | 3.0.2 (apt `libssl3` 3.0.2-0ubuntu1.23) | 2022-03 / 패치 2024 | **3.5.6 LTS** | 2026-04-07 | 4년 / 5 minor | apt | 중 — 3.0 LTS EOL ~2026-09. 3.5 LTS = 2030-04 까지 |
| **jemalloc** | 5.2.1 (apt `libjemalloc2` 5.2.1-4ubuntu1) | 2019-08 | **5.3.1** | 2026-04-13 | 6.5년 / 1 minor | apt | 소 — 24.04 or 소스 |
| **restclient-cpp** | 0.5.3 (소스 빌드, Dockerfile.build:86) | 2025-01-02 | **0.5.3** | 2025-01-02 | ✅ latest | 소스 | 0 |
| **prometheus-cpp** | v1.3.0 (소스 빌드, Dockerfile.build:118) | 2024-11-03 | **v1.3.0** | 2024-11-03 | ✅ latest | 소스 | 0 |
| **librknnrt** | 1.5.2 (host mount `code/Engine/NPU/librknn_api/aarch64/librknnrt.so`, 빌드 `c6b7b351a@2023-08-23`) | 2023-08-23 | **2.3.2** (RKNN-Toolkit2 v2.3.2 동봉) | 2025-04-09 | 2년 / **major 1→2** | host | **대 — .rknn 모델 전부 재변환 + Engine/NPU 코드 API 검증 필요** |

## 핵심 관찰
1. **이미 최신 (조치 불필요)**: restclient-cpp, prometheus-cpp — Dockerfile.build 가 git tag pin 으로 최신 stable 유지.
2. **Ubuntu base image 24.04 bump 1회로 일괄 획득**: GStreamer 1.20→1.24, libcurl 7.81→8.5, jemalloc 5.2→5.3. OpenSSL 은 24.04 도 3.0 line 유지.
3. **소스 빌드 강제 (현재 진짜 최신 원할 시)**: GStreamer 1.28, libcurl 8.20, OpenSSL 3.5.
4. **librknnrt 가 가장 stale + major bump 부담 큼** — 별도 epic 으로 분리 권장. 자세히는 §librknnrt 섹션.

## 보안 관점
현 Ubuntu 22.04 apt 의 libcurl 7.81 / OpenSSL 3.0.2 / GStreamer 1.20.3 는 **canonical security 패치 적용 중** (libcurl `7.81.0-1ubuntu1.24` = 24번째 patch). 즉시 CVE 노출 아님. 다만 4년+ stale 한 major line — 단/중기 bump 계획 가치 있음.

## 우선순위 권장
1. **즉시 (0 비용)**: 이미 최신 (restclient-cpp / prometheus-cpp).
2. **저비용 minor bump**: jemalloc 5.2 → 5.3.
3. **중비용 base image bump**: Ubuntu 22.04 → 24.04 시 GStreamer 1.24 + libcurl 8.5 + jemalloc 5.3 동시. RTSP 동작 검증 필수.
4. **신중 검토 (audit + 모델 재변환 강제)**: librknnrt 1.5 → 2.3. v1.0.0 release 후 별도 epic.
5. **선택**: OpenSSL 3.5 LTS — 3.0 LTS EOL (~2026-09) 가까워질 때.

## §librknnrt 1.5.2 → 2.3.2 상세

### 현 상태
- 모델 파일 3개 (`engines/`): `yolov5s_airockchip.rknn` (8MB), `yolov5m_airockchip.rknn` (24MB), `yolov5l_airockchip.rknn` (49MB)
- 모두 **airockchip 공식 변환본** (`_airockchip` 접미사). 우리가 직접 변환한 게 아님.
- 원본 `.pt` / `.onnx` 는 repo 에 없음 (필요 시 airockchip GitHub 에서 받음).

### ".rknn 모델 재변환" 의 의미
NPU 모델은 **3단계 binary 변환**을 거친다:
1. `.pt` (PyTorch native, 학습용) → `.onnx` (ONNX, 표준 교환) → `.rknn` (Rockchip 전용 binary)
2. `.rknn` 안에는 quantization + operator fusion + NPU scheduling 이 **미리 컴파일**되어 있음
3. **`.rknn` binary format 자체가 librknnrt 메이저 버전에 묶여있다** — librknnrt 1.5 가 만든 `.rknn` 은 librknnrt 1.x 에서만 load. librknnrt 2.x runtime 은 새 internal IR + operator format 사용 → 1.x `.rknn` 못 읽음.

재변환 절차 (일반):
1. 원본 `.onnx` 보유 → **RKNN-Toolkit2 v2.3.2** (Python tool) 로 다시 변환
2. **calibration dataset 으로 quantization 재진행** (INT8 quantization 시 정확도 보존용 image set 필수)
3. 새 `.rknn` 산출 → `engines/` 의 기존 파일 교체

DetectBase 의 현실 (간단함):
- airockchip 가 **toolkit2 v2.x 호환 yolov5_airockchip.rknn 을 release 했는지 확인** → 다운받아 교체. 직접 변환 path 불필요.
- 직접 변환 path 가 필요한 경우 (custom 모델 도입) RKNN-Toolkit2 Python tool 별도 설치 + calibration dataset 준비 필요.

### bump 의 이득 (1.5 → 2.3 누적)

CHANGELOG 분석 결과:

| 영역 | 1.5 → 2.3 변경 | DetectBase 영향 |
|---|---|---|
| **Operator fusion** (v2.1+) | NPU 그래프 컴파일 효율 강화 | YOLOv5 inference **+5~15% 속도 추정** |
| **Quantization 개선** | int4*int4→int16 (v1.6, RK3588), W4A16 symmetric (v2.3, RK3576), automatic mixed precision (v2.3) | 더 작은 모델, 더 빠른 inference, 정확도 보존 |
| **Transformer 지원** | SDPA (v2.0), Flash Attention (v2.1, RK3576/3562) | YOLOv8/v11, RT-DETR, YOLO-NAS 같은 신규 모델 사용 path 열림 |
| **ONNX OPSET** | 12 → 19 (v1.6) | 더 새 onnx export 모델 import 가능 |
| **PyTorch 호환** | PyTorch 2.1 (v2.0+) | 신규 학습 코드 직접 변환 가능 |
| **device DMA / zero-copy** | path 개선 누적 | 대용량 batch / multi-cam 시 latency 안정성 |
| **stability bug fix** | 2년치 누적 | 현 librknnrt 1.5.2 known issue (특히 batch / device race) 해결 가능성 |

### bump 시 검토 항목
1. **API breaking 검토**: v0.1.27 NPU batch_size 확장 patch (`code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/`) 가 librknnrt 1.x API 기반. 2.x 가 batch 다루는 방식이 달라졌으면 patch 재작성 필요. (일반적으로 더 깔끔해질 가능성).
2. **모델 파일 재공급**: airockchip 의 v2.x 호환 `_airockchip.rknn` 다운 + engines/ 교체
3. **host librknnrt.so 교체**: `code/Engine/NPU/librknn_api/aarch64/librknnrt.so` 를 v2.3.2 파일로 교체
4. **audit + 모니터**: ASan/TSan + 6시간 + DFPS 비교 (개선 측정)

### 권장 timing
v1.0.0 release 안정화 후 **별도 epic** 으로 진행. v2.0.0 multi-engine 계획과 묶을 수 있음 (newer YOLO model 채택 path 열림).

### 엔진팀 요청 — 한 줄
사내 AI 엔진팀이 모델 + .rknn 컴파일 담당. **요청 내용 = "rknn-toolkit2 메이저 버전 올려서 동일 spec (YOLOv5 / 640×640 / COCO 80 / INT8 affine / 3-output detection-head-stripped / anchor 값 동일) 으로 재컴파일"** 그 한 줄. 그 외 모델 변경 없음.

우리 측 동시 작업 (엔진팀 요청 외, self): librknnrt.so host mount 교체 (엔진팀이 알려준 권장 runtime 버전) + 새 .rknn 설치 + audit + 모니터.

## 출처
- [GStreamer 1.28.3 (Linuxiac/Linux.org)](https://www.linux.org/threads/linuxiac-gstreamer-1-28-3-released-with-security-and-playback-fixes.66325/)
- [curl Release Table](https://curl.se/docs/releases.html)
- [OpenSSL endoflife.date](https://endoflife.date/openssl)
- [OpenSSL 3.5 LTS](https://openssl-library.org/post/2025-02-20-openssl-3.5-lts/)
- [jemalloc 5.3.1 (GitHub)](https://github.com/jemalloc/jemalloc/releases/tag/5.3.1)
- [rknn-toolkit2 v2.3.2 (GitHub)](https://github.com/airockchip/rknn-toolkit2/releases/tag/v2.3.2)
