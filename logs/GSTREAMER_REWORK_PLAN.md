# GStreamer 재작업 plan + 이전 시도 회고

**작성일**: 2026-05-15
**대상**: feature/gstreamer-integration branch — GStreamer 통합 재시도
**전제**: happytimesoft baseline + jemalloc 안정 (24h test 진행 중, 결과 통과 시 진입)

---

## §1. 이전 시도 회고 (2026-05-14 ~ 15, 롤백됨)

### 1.1 시도 코드 구조 (`.deleted_backup/gst_attempt_20260515/RTSP_GST/`)

총 1,437 LOC, 4 클래스 + 1 helper:

| 파일 | LOC | 책임 |
|---|---|---|
| `GstRtspClient.{h,cpp}` | 235 | rtspsrc + decode 의 client wrapper, reconnect 로직 |
| `GstRtspProxyServer.{h,cpp}` | 296 | gst-rtsp-server 기반 분석 결과 RTSP proxy 출력 |
| **`GstRtspReceiver.{h,cpp}`** | **412** | **pipeline build + bus watch + state management — leak 의심 가장 큼** |
| `EventRecorder.{h,cpp}` | 296 | 영상 녹화 (옵션) |
| `OnvifMetadataPayloader.{h,cpp}` | 198 | ONVIF metadata RTP 페이로더 자체 구현 (240 LOC 목표) |

추가: `RTSP_shim/` — happytimesoft API 와 일치하는 shim 으로 `RtspHandler.cpp` 의 변경 최소화 시도.

### 1.2 측정한 leak 양 (BISECT 결과)

| 구성 | KB/reconn | 비고 |
|---|---|---|
| avdec_h264 1h sanity (2026-05-14 PoC) | **0.07** | production grade — PoC 14/14 PASS |
| mppvideodec + full pipeline restart | 12 | mpp instance 재생성 leak |
| mppvideodec + in-place reset | 5.5 | partial fix |
| 2-pipeline (mpp 영구) | 5~9 | architecture 복잡 |
| BISECT7 (mppvideodec 자체 제거) | 1.4 | source pipeline 만 운영 |
| BISECT8 (avdec_h264 재현, frame_cb skip) | 1.7 | reset cycle 자체 영향 |
| 단일 pipeline + avdec_h264 (롤백 후) | 0.68 | 이전 0.07 보다 10배 |

**결론**: 4 cam × 매 reconnect 마다 누적 → 24h 운영 시 수 MB ~ 수 십 MB 추가 누수. production 부적합.

### 1.3 진단 도구 한계 (체험)

| 도구 | 결과 | 한계 |
|---|---|---|
| ASan | malloc 만 추적 | mmap 영역 안 잡힘 (jemalloc/MPP 의 직접 mmap 누락) |
| valgrind massif | 100x slow | DetectBase 가 운영 안 됨 (DFPS 0, NPU 요청 0) — `.deleted_backup/mpp_rollback/massif_run.log` 가 증거 |
| `GST_TRACERS=leaks` | SIGUSR1 dump 무동작 | tracer init 만 됐고 leak summary 출력 X (`.deleted_backup/gst_tracer_v*_*.log` 참조) |
| jemalloc prof | apt 패키지 prof 미지원 | `Invalid conf pair: prof:true` 에러. source build 필요 |
| `/proc/PID/smaps` | region 보이지만 stack X | leak 한 함수 미식별 |
| eBPF (시도 안 함) | bpftrace 미설치 | 다음 시도 시 고려 |

### 1.4 실패 원인 추정 (확정 X — 진단 도구 한계로 미식별)

가능성 (시간순):

1. **reconnect path 의 buffer/element ref leak** — BISECT 결과 reconnect 자체가 trigger. 어디서 ref 가 1 남는지 미확인.
2. **MPP decoder instance 재생성** — `mppvideodec` 가 reset 마다 internal context 재할당 (BISECT 12 KB/reconn 으로 가장 큼).
3. **rtspsrc 의 jitter buffer / RTP session 잔재** — 재연결 후 이전 session 의 buffer 가 유지되는 의심.
4. **GLib slice allocator + jemalloc 의 상호작용** — GLib 가 자체 slice pool 사용 → jemalloc 회수 메커니즘 밖. 측정 RSS 가 과대 보일 가능성도.

---

## §2. GStreamer 환경의 기본 RSS 증가 원인 분석 (사용자 질문 답)

happytimesoft baseline (~528 MB) 와 GStreamer 시도 (~600~700 MB 추정) 의 default RSS 차이 ~70~170 MB. 원인 추정 (개별 효과 합산):

### 2.1 라이브러리 / runtime 로딩 (가장 큼, ~40~80 MB)

| 항목 | 추정 메모리 | 이유 |
|---|---|---|
| `libgstreamer-1.0.so.0` + base | ~15 MB | core 의 GValue, GstElement, GstBuffer 등 type system |
| `libgobject-2.0.so.0` + `libglib-2.0.so.0` | ~10 MB | GObject 시스템 + GMainLoop + GHashTable 등 |
| `libgstrtsp-1.0.so.0` + `libgstrtspserver-1.0.so.0` | ~8 MB | RTSP server/client + SDP parser |
| `libgstapp-1.0.so.0`, `libgstvideo-1.0.so.0`, `libgstaudio-*` | ~10 MB | appsrc/sink + video/audio helpers |
| plugin loading (rtspsrc, decodebin, avdec_h264, mppvideodec, x264enc, queue, tee, ...) | ~10~25 MB | 각 plugin 의 init + factory 등록 |

→ **library + plugin 만 ~50 MB**.

### 2.2 GLib slice allocator (~10~30 MB)

- GLib 가 자체 slice pool 사용 (`GSlice`)
- 작은 객체 (GstBuffer header, GstEvent, GstQuery, 등) 모두 slice 로 할당
- jemalloc 의 `madvise(MADV_DONTNEED)` 회수가 slice 안에서는 안 일어남 (slice 자체가 jemalloc 위 한 chunk)
- N pipeline (cam 4 대) × M elements per pipeline × 객체 수 → slice pool 이 1회 부풀면 그 크기 유지
- 환경변수 `G_SLICE=always-malloc` 로 jemalloc 으로 우회 가능 (성능 trade-off)

### 2.3 pipeline 의 buffer pool / queue (~10~30 MB)

| 항목 | 추정 |
|---|---|
| `rtspsrc` jitter buffer (default 2000 ms × bitrate) | ~2~5 MB/cam |
| `queue` element default (200 buffers × frame size) | ~5~10 MB/cam |
| `decodebin` internal frame buffer (decoder reference frames + DPB) | ~5~10 MB/cam |
| H.264/H.265 decoder DPB (decoded picture buffer) | ~3~5 MB/cam |

4 cam 환경에서 ~5~30 MB 추가.

### 2.4 MPP decoder buffer pool (mppvideodec 사용 시, ~30~80 MB)

- MPP NPU decoder 가 internal frame pool 보유 (DRM/ION buffer)
- 1080p NV12 frame ~3 MB × 8~16 reference frames + decode buffer × N cam
- 4 cam × 8 buffer × 3 MB = ~96 MB (worst case)

→ 이게 MPP 통합 시 RSS 가 가장 크게 늘어나는 항목.

### 2.5 새 thread stack (~5~15 MB)

- GStreamer 가 추가 thread 생성:
  - per pipeline: streaming thread, bus watch thread, source pad task
  - rtspsrc: RTCP thread, UDP receiver thread
- 4 cam × ~5 thread = +20 thread
- 각 thread default stack 8 MB virtual (RSS 는 실제 사용량 — 보통 64~256 KB)
- 실 RSS ~5 MB 이내, 다만 VmSize/VmPeak 는 ~160 MB 증가 가능

### 2.6 libav (avdec_h264 사용 시, ~5~15 MB)

- `libavcodec` / `libavformat` / `libavutil` 의 internal state
- decoder context per stream (~3 MB × 4 cam)

### 2.7 회피/완화 방법

| 방법 | 효과 | trade-off |
|---|---|---|
| `G_SLICE=always-malloc` | GLib slice → jemalloc redirect → 회수 가능 | 성능 미세 저하 (~1~2%) |
| pipeline `queue max-size-buffers` 조정 | buffer pool 크기 ↓ | underrun 위험 |
| `rtspsrc latency` 줄이기 | jitter buffer ↓ | network jitter 민감 |
| MPP buffer count 조정 | DPB ↓ | decode pipeline depth ↓ |
| H.264 only (H.265 plugin 미로딩) | plugin 라이브러리 ~3~5 MB ↓ | H.265 카메라 사용 불가 |

**결론**: GStreamer + MPP 통합 시 default RSS 가 +100~150 MB 정도 증가는 **정상 범위**. 단 plateau 도달 후 안정해야 함 (이전 시도는 안정 X → reconnect 마다 누적).

---

## §3. 재작업 단계별 plan

각 단계는 별도 commit + sanity 검증 후 진입. 단계 하나라도 fail 시 stop.

### Phase 1: avdec_h264 단일 pipeline PoC (1~2일)

- 목표: happytimesoft 의 RTSP receiver 만 GStreamer 로 교체. decoder = soft (`avdec_h264`). 분석 결과 출력 (RTSP proxy) 은 happytimesoft 유지.
- 기준점: 2026-05-14 의 PoC (0.07 KB/reconn). 그 결과 재현.
- 변경: `RtspHandler.cpp` 가 happytimesoft `CRtspProxy` 대신 `GstRtspClient` 호출.
- 빌드: Dockerfile.build 에 libgstreamer-* + plugins-good + libav 추가.
- 검증: PoC 14 case + 1h reconnect sanity (목표: 0.5 KB/reconn 이내).

### Phase 2: video forward (raw passthrough) 통합 (1일)

- 목표: GStreamer pipeline 안에서 NPU inference 위한 raw frame 추출 (`appsink`).
- 변경: avframe_q 의 producer 가 happytimesoft 의 RTP rx → `appsink` 로 변경.
- 기존 sws_scale 등 모든 frame 처리 그대로 (consumer 변경 X).
- 검증: DFPS 50+, ERROR 0 1h.

### Phase 3: 분석 결과 RTSP proxy 출력 (2일)

- 목표: 분석 결과 (overlay 등) 출력도 GStreamer (`gst-rtsp-server` 또는 `rtspclientsink`) 로 교체.
- 변경: `RtspHandler` 의 RTSP server 부분 → `GstRtspProxyServer`.
- ONVIF metadata 페이로더는 자체 구현 (`OnvifMetadataPayloader`, 198 LOC).
- 검증: 외부 viewer (VLC) 로 분석 결과 수신, ONVIF metadata 정상 송신.

### Phase 4 (선택): MPP hardware decoder 통합 (3~5일)

- 목표: avdec_h264 → mppvideodec (NPU 하드웨어 가속).
- **이전 시도의 leak 위치 (정확히 미식별)** 가 재현될 가능성 — 별도 분석 필수.
- 사전 작업:
  - bpftrace 또는 systemtap 설치 (`madvise`, `mmap` syscall 추적)
  - jemalloc source build (`--enable-prof` flag) 로 prof 활성화
  - GST_TRACERS leaks 의 dump 시그널 동작 검증 (이전엔 무동작)
- 미식별 시 Phase 4 보류 + avdec_h264 만으로 production 운영.

---

## §4. 각 단계 검증 점

| 단계 | DFPS | 1h Δ RSS | reconn KB/reconn | ERROR | 추가 |
|---|---|---|---|---|---|
| Phase 1 | 50+ | < +50 MB | < 0.5 | 0 | PoC 14/14 PASS 재현 |
| Phase 2 | 50+ | < +20 MB | < 0.5 | 0 | event 발생 정상 |
| Phase 3 | 50+ | < +30 MB | < 0.5 | 0 | VLC 수신 정상 + ONVIF metadata |
| Phase 4 | 50+ | < +20 MB | < 1.0 | 0 | MPP leak 위치 식별 + fix |

각 단계마다 `scripts/run_48h_test.sh` 또는 24h 변형으로 sanity.

## §5. rollback trigger

다음 발생 시 즉시 해당 Phase rollback (이전 단계로):

- KB/reconn > 5 (3시간 sanity 평균)
- RSS 1h Δ > 100 MB
- DFPS < 50 30분 이상
- ERROR > 10/h
- 미식별 leak 의 측정 도구 한계 (MPP 시도 시)

rollback 은 git revert 또는 branch 이전 commit checkout 으로 수행. master 머지 전이므로 history rewrite 없음.

## §6. 자료 위치 reference

- 이전 시도 코드: `.deleted_backup/gst_attempt_20260515/RTSP_GST/`
- 이전 PoC: `.deleted_backup/code-gstreamer-poc_20260515/day0/`
- valgrind massif: `.deleted_backup/mpp_rollback_20260515/massif_run.log`
- GST_TRACERS logs: `.deleted_backup/gst_tracer_v[12]_*.log`, `.deleted_backup/mpp_rollback_20260515/gst_tracer.log`
- ASan 빌드: `.deleted_backup/mpp_rollback_20260515/Build-ASan/`
- 빌드 logs: `logs/build_mpp_*.log`, `logs/build_v3_*.log`, `logs/gstreamer_phase0_*.log`, `logs/jemalloc_rebuild_*.log`
- cam별 isolation: `logs/gst_diag_160804/gst_cam[1-4].log`
- 자체 ONVIF 페이로더 설계: `logs/ONVIF_PAYLOADER_DESIGN.md`
- 단계별 심층 review: `logs/GSTREAMER_DEEP_REVIEW.md`
