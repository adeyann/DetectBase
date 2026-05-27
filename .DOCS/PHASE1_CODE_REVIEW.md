# Phase 1 코드 심층 리뷰 (2026-05-15)

GStreamer Phase 1 (rtspsrc + avdec_h264, MPP 없음) 의 코드 작업 완료
직후 build/sanity 검증 전에 실시한 심층 리뷰 기록. 24h baseline test
가 진행 중이라 빌드/실행이 불가한 상태에서 정적 점검 + 의심 영역 분석
만 수행. fix 한 건 + 보류 항목 + 24h 이후 검토 항목을 모두 기록.

---

## §1. 리뷰 scope

- **branch**: `feature/gstreamer-integration`
- **검토 commits**: `a67af5c` → `72113e1` → `6b09b96` → `c661bc2` → `69cb032` → `301cfa8`
- **변경량**: −50,246 LOC (happytimesoft + tinyxml 제거), +1,402 LOC (RTSP_GST + RtspHandler 재작성 + Unit 통합)
- **리뷰 일시**: 2026-05-15 22:xx KST
- **빌드 상태**: 미검증 (24h baseline test 진행 중이라 docker rebuild 대기)

---

## §2. 점검한 영역 (정적 검증)

| 항목 | 영역 | 결과 |
|---|---|---|
| A | `SafeQueue.h` include path 정합 | ✓ BasicLibs PUBLIC propagation |
| B | `CameraSettingsManager::GetSetting` 시그니처 (`std::optional<T>`) | ✓ |
| C | `MGEN::GetSettingManager()` 시그니처 (`inline shared_ptr<SettingManager>`) | ✓ |
| D | `GstRtspClient` ctor + Start 동작 | ✓ |
| E | `RTSP_GST` CMakeLists 의 BasicLibs PUBLIC link | ✓ |
| F | `GstRtspReceiver::BuildPipeline` 의 GstElement ref 관리 (gst_object_unref) | ✓ |
| G | `AVFrame` deleter 패턴 (`shared_ptr` deleter 가 `av_frame_free`) | ✓ |
| H | `gst_init` 중복 호출 안전성 (idempotent guard 있음) | ✓ |
| I | docker-compose 환경 (`GST_PLUGIN_PATH` 등 미설정, default 작동) | ✓ |
| J | `code/CMakeLists.txt` subdirectory 순서 | ✓ (BasicLibs → RTSP_GST → Management → Main) |
| K | `GstRtspClient.cpp` include 정합 | ✓ |
| L | `GstRtspReceiver.cpp` include 정합 | ✓ |
| M | BasicLibs PUBLIC include propagation | ✓ |
| N | `MetricsRegistry::Instance()` static singleton 사용 | ✓ |
| O | `ProxyVideoInfo` 사용처 (RtspHandler 내부만) | ✓ |
| P | `GST_DEBUG` 환경변수 | 미설정 (default WARN, 첫 sanity 시 임시 `GST_DEBUG=2` 권장) |

---

## §3. 발견 항목 + 결정

### Q1 — CRITICAL — `ConvertSampleToAVFrame` 의 buffer-size 부족 시 garbage frame 반환

**위치**: `code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp:332~351` (수정 전)

**문제**:
```cpp
if( map.size >= y_sz + u_sz + v_sz ) {
    std::memcpy( frame->data[0], src, y_sz );      // ← size 부족하면 SKIP
    std::memcpy( frame->data[1], src + y_sz, u_sz );
    std::memcpy( frame->data[2], src + y_sz + u_sz, v_sz );
}
gst_buffer_unmap( buf, &map );
// ⚠️ size 부족해도 frame 반환됨 — Unit/NPU 에 garbage 데이터 전달
return std::shared_ptr<AVFrame>( frame, ... );
```

**위험**:
- map.size 가 부족하면 `av_frame_get_buffer` 가 할당했으나 데이터 미설정된 frame 이 반환됨.
- Unit 의 `avframe_q_` → NPU inference 까지 흘러감 → noise / 잘못된 detection / 최악의 경우 segfault.

**수정**: commit `301cfa8` (2026-05-15)
- short buffer 검출 시 `av_frame_free` + `gst_buffer_unmap` + `nullptr` 반환.
- `OnNewSample` 가 이미 nullptr 처리 (callback skip) 하므로 queue / NPU 는 영향 X.

**상태**: ✅ FIXED

---

### Q2 — LOW — `ReconnectWorker` 의 무한 재시도

**위치**: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp:222~224`

**동작**:
- `StartReceiver` 실패 시 `backoff_sec_` 가 max (60s) 까지 증가하면서 영원히 재시도.
- 카메라 영구 오프라인 시 분당 1회 reconnect 시도 → `detectbase_gst_rtsp_client_reconnect_total` metric 영원히 증가.

**위험**:
- sanity 시 false positive leak signal (실제 leak 가 아니라 metric noise).
- 4 cam 중 1대만 long-term down 이면 다른 3대의 정상 metric 노이즈 가능.

**결정**: **수용** — design 의도 (자동 복구). 24h sanity 후 metric noise 측정 → 실제 문제면 max retry limit 추가.

---

### Q3 — LOW — `ResetSourceOnly` 의 BISECT8 +1.7 KB/reconn

**위치**: `code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp:229~249`

**문제**:
- 이전 시도 (`.deleted_backup/gst_attempt_20260515/`) 의 BISECT 결과 BISECT8 (mppvideodec 자체 제거, avdec_h264 + reset cycle 만) 가 1.7 KB/reconn leak 발생.
- 즉 `TeardownPipeline + BuildPipeline` cycle 자체에 어딘가 ref leak.
- 정확한 unref 누락 위치는 진단 도구 한계 (ASan/valgrind/GST_TRACERS 모두 미식별) 로 식별 X.

**결정**: **수용** — Phase 1 acceptable 수준 (0.07 KB/reconn 까지는 production grade, 1.7 KB 는 그 10배 정도지만 24h 누적 최악 100 reconnect 가정해도 +170 KB). 24h sanity 의 RSS delta 가 1 MB/h 이내면 무시.

**관련**: Q18 (in-place reset 자체 제거 시 이 leak risk 감소 가능성).

---

### Q4 ~ Q9 — 점검 + OK

| Q | 항목 | 결과 |
|---|---|---|
| Q4 | `METRIC_FRAMES_TOTAL` / `METRIC_RECONNECT_TOTAL` / `METRIC_ERRORS_TOTAL` / `METRIC_ENQUEUE_DROP_TOTAL` 정의 | ✓ anonymous namespace 안 정의 (Receiver/Client 각각) |
| Q5 | Stop 순서 (DETECTOR.cpp 의 #01 Detector Block → ... → #04 Network Flow → #05 IOStream) | ✓ Unit thread join 이 RtspHandler::StopRTSP 보다 먼저 |
| Q6 | `receiver_mtx_` vs main loop callback thread race | ✓ Stop() 안 `g_main_loop_quit` + `join` 으로 callback 종료 보장 |
| Q7 | `METRIC_FRAMES_TOTAL` 정의 위치 | ✓ |
| Q8 | `NO_LABELS` 정의 | ✓ `const std::map<std::string, std::string> NO_LABELS;` |
| Q9 | `ReconnectWorker` 의 `cv_.wait` predicate | ✓ atomic load 사용, spurious wakeup 안전 |

---

### Q10 — LOW — `frame_cb` 의 drop detection race

**위치**: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp:117~124`

**동작**:
```cpp
const size_t before = queue_->size();
queue_->enqueue_move( std::move( frame ) );
if( before >= cfg_.queue_max_size ) {
    enqueue_drop_count_.fetch_add( 1 );
}
```

**문제**:
- `queue_->size()` 와 `enqueue_move` 사이 다른 thread (consumer) 의 dequeue 발생 가능.
- `before == max - 1` 이지만 enqueue 시점에 capacity 가득 → drop 발생했는데 카운터 increment X (miss).
- `before == max` 이지만 enqueue 직전 consumer 가 dequeue → drop 안 났는데 카운터 increment (false positive).

**결정**: **수용** — minor metric noise. 운영 영향 거의 0. 더 정확한 drop detection 이 필요하면 SafeQueue 에 `enqueue_with_drop_count` API 추가 (Phase 2).

---

### Q11 — LOW — `gst_client->Start()` 실패 시 `proxy_ptr_` 처리

**위치**: `code/Main/DETECTOR/src/RtspDetectorUnit.cpp:813`

**동작**:
- `gst_client->Start()` 실패 시 `return` (InferenceThread 종료) → `proxy_ptr_` 가 set 안 됨 (기본값 `nullptr`).
- Phase 1 에서 `proxy_ptr_` 사용 0건 → 무해.

**결정**: **수용**.

---

### Q12 — `RtspHandler::RegisterClient` 의 기존 cam_id 교체

**위치**: `code/Management/worker/src/RtspHandler.cpp:101~113`

**결과**: ✓ OK (기존 client Stop + erase + 새 등록, 단일 thread 흐름)

---

### Q13 — `%lu` vs `uint64_t` 정합 (aarch64 Linux)

**위치**: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp:87`

**결과**: ✓ OK (aarch64 Linux 의 `unsigned long` = 64 bit = `uint64_t`)

---

### Q14 — `GstRtspClient::Stop` sequence

**위치**: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp:75~91`

**결과**: ✓ OK (`shutdown_` set → `cv.notify` → `reconnect_thread.join` → `StopReceiver`)

---

### Q15 — `enable_raw_passthrough = false` default

**결과**: Unit cfg 에 명시 X → default `false` 사용. Phase 1 의도 정합.

---

### Q16 ~ Q17 — rtspsrc 설정 (`latency=200ms`, `timeout=2s`, `tcp-timeout=5s`)

**결과**: LAN 환경 default 충분. 외부 viewer / 인터넷 카메라 환경에서 tune 필요 시 Phase 3 영역.

---

### Q18 — MEDIUM — `ResetSourceOnly` 의 EOS in-place reset 경로 (Phase 1 motivation 없음)

**위치**: `code/Protocol/RTSP_GST/src/GstRtspClient.cpp:178~198`

**동작**:
```cpp
const bool is_eos = eos_reconnect_pending_.exchange( false );
if( is_eos ) {
    // In-place reset 시도 (receiver_ 보존)
    bool inplace_ok = false;
    { lock; if( receiver_ ) inplace_ok = receiver_->ResetSourceOnly(); }
    if( inplace_ok ) { ... continue; }   // mppvideodec 보존
    // fallback: 아래 full restart path 로
}
// full restart: StopReceiver + StartReceiver
```

**문제**:
- 이 in-place reset 경로는 mppvideodec 사용 시 mpp instance 의 internal hardware DMA buffer leak (~12 MB/reconn) 회피 위해 도입됨.
- Phase 1 = `avdec_h264` (CPU 디코더, MPP 없음) → in-place reset 의 motivation 자체가 없음.
- 게다가 `ResetSourceOnly` 자체가 `TeardownPipeline + BuildPipeline` 이고, 그 cycle 이 BISECT8 의 +1.7 KB/reconn 의 의심 영역 (Q3).

**잠재 fix**:
- EOS 도 full restart 으로 단순화 (`StopReceiver + StartReceiver`).
- `ResetSourceOnly` 함수 자체 제거 또는 deprecated.
- 코드 ~30 lines ↓, branch 복잡도 ↓.

**결정**: **대기** — 24h sanity 후 leak 패턴 (KB/reconn) 측정 + 사용자 결정. design 변경 가치 있지만 명확한 leak 감소 보장 X (BISECT8 의 1.7 KB 가 ResetSourceOnly 의 in-place path 에서 발생인지 full restart path 에서 발생인지 미식별).

**24h sanity 후 trigger 조건**:
- Phase 1 build OK + sanity 시 KB/reconn 측정.
- 만약 > 2 KB/reconn → Q18 fix 진행 권장.
- 만약 < 0.5 KB/reconn → Phase 1 acceptable, Q18 fix 보류 (Phase 4 MPP 통합 시 다시 검토).

---

### Q19 — `SettingMonitor` 의 runtime camera 변경 처리

**현재 상태**:
- Phase 1 minimal 에서 정적 init data 만 처리 (Unit::Init 시 한 번 RegisterClient).
- SettingMonitor 의 callback 이 runtime 에 카메라 추가/제거 trigger 시 RtspHandler 에 직접 호출 안 함.

**결과**: Phase 2 영역. SettingMonitor 와 RtspHandler 의 wiring 추가 필요.

---

### Q20 — `MGEN::GetSettingManager()` singleton thread-safety

**결과**: ✓ OK (read-only after init)

---

## §4. 24h sanity 시 확인할 영역

Phase 1 build 후 (24h baseline test 종료 시) 검증:

1. **Build 검증** — 컴파일 에러 식별 + fix
2. **gst_init log 출력** — `RtspHandler: gst_init done (GStreamer X.Y.Z)` 정상
3. **rtspsrc connect 확인** — `MLOG_INFO( "CAM[%d]::InferenceThread: GstRtspClient registered (url=...)" )` 4 cam 정상
4. **DFPS 안정성** — ≥ 50 (4 cam × 13 fps)
5. **카메라 active count** — 4/4
6. **ERROR/WARN 누적** — 0 (또는 사용 가능 수준)
7. **VmRSS 1h delta** — 목표 < 5 MB/h (이전 시도 0.07 KB/reconn × N 이내)
8. **Metric 추세**:
   - `detectbase_gst_rtsp_client_reconnect_total` — 정상 운영 시 0 또는 minimum
   - `detectbase_gst_rtsp_client_enqueue_drop_total` — 0 또는 minimum
   - `detectbase_gst_rtsp_frames_total` — DFPS × 60s × N cam 비례 증가
9. **KB/reconn** — 강제 reconnect 1~2회 trigger 후 측정 (이전 시도 BISECT8 = 1.7 KB/reconn)
10. **외부 viewer 호환성** — Phase 3 영역 (Phase 1 에선 미해당)

---

## §5. fix history

| Commit | 일시 | 항목 |
|---|---|---|
| `301cfa8` | 2026-05-15 22:xx | Q1 fix — `ConvertSampleToAVFrame` returns `nullptr` on short buffer |

---

## §6. 보류 fix list (24h sanity 후 검토)

| Q | 항목 | 트리거 조건 |
|---|---|---|
| Q2 | `ReconnectWorker` max retry limit | 영구 down 카메라로 인한 metric noise 확인 시 |
| Q18 | `ResetSourceOnly` 제거 (Phase 1 motivation 없음) | sanity KB/reconn > 2 KB 또는 design 단순화 결정 시 |
| Q19 | `SettingMonitor` → `RtspHandler` runtime camera 변경 wiring | Phase 2 (runtime 설정 변경 처리 필요 시) |
| Q10 | `frame_cb` drop detection 정확화 | metric noise 가 운영 문제 발생 시 |

---

## §7. 참고 자료

- BISECT 결과 표: `.DOCS/GSTREAMER_REWORK_PLAN.md` §1.2
- 이전 시도 코드: `.deleted_backup/gst_attempt_20260515/RTSP_GST/`
- valgrind massif log: `.deleted_backup/mpp_rollback_20260515/massif_run.log`
- GST_TRACERS log: `.deleted_backup/gst_tracer_v[12]_*.log`

---

## §8. 결론

- **Critical fix 0건 (Q1 처리 완료)** — Phase 1 코드는 정적 점검 기준 production-ready 후보.
- **MEDIUM 1건 (Q18) — 보류** — 24h sanity 결과 보고 결정.
- **LOW 4~5건 — 모두 수용** — 운영 영향 거의 0, design 의도 또는 minor metric noise.
- **빌드 / runtime 검증은 24h test 종료 후** (`logs/test_24h_20260515_165937/SUMMARY.md` 분석 시점).
- Phase 1 진입 결정은 sanity 결과 (DFPS / RSS / ERROR / KB/reconn) 검토 후.
