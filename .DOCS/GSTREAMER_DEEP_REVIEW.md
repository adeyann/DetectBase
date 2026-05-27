# GStreamer PoC 단계별 심층 재검토

> **STATUS (2026-05-15)**: GStreamer + MPP hardware decoder 통합 시도 → **롤백**.
> 매 reconnect 시 RSS leak ~10 MB/reconn 발견, 모든 진단 도구 (ASan/valgrind/GST_TRACERS/jemalloc prof) 한계 도달.
> 시도 코드는 `.deleted_backup/gst_attempt_20260515/` 에 보존. 향후 GStreamer 재작업 branch (`feature/gstreamer-integration`) 에서 재개 시 본 문서 참고. 진행 흐름은 [logs/NEXT_SESSION.md](NEXT_SESSION.md) 참조.

---

**작성일**: 2026-05-13
**대상**: NEXT_SESSION.md 의 [3] Phase 0~6 비판적 재검토
**참조**: ONVIF 페이로더 자체 검토는 [ONVIF_PAYLOADER_DESIGN.md §13](ONVIF_PAYLOADER_DESIGN.md) 에 별도 정리됨

---

## Phase 0 심층 — Dockerfile + 의존성

### 0.1 🟡 빠진 apt 패키지 — 추가 검증

현재 NEXT_SESSION.md 의 패키지 목록:
```
libgstreamer1.0-dev
libgstrtspserver-1.0-dev
gstreamer1.0-plugins-base
gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad
gstreamer1.0-libav
gstreamer1.0-tools
```

**누락 의심 패키지:**

| 패키지 | 필요 이유 |
|---|---|
| `libgstreamer-plugins-base1.0-dev` | base plugin 개발 헤더 (`gst/app/gstappsrc.h` 등). 일부 시스템에선 자동 의존, 명시 안전 |
| `libglib2.0-dev` | GLib (GStreamer 의 기반 객체 시스템). 자동 의존 가능 |
| `gstreamer1.0-rtsp` | RTSP 클라이언트 (rtspsrc) — `plugins-good` 에 포함되어 있지만 명시 안전 |
| `gobject-introspection` | gi 도구 (필요 시) |

**권장 변경:**
```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        libgstrtspserver-1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-libav \
        gstreamer1.0-tools \
    && rm -rf /var/lib/apt/lists/*
```

### 0.2 🟡 빌드 시스템 (CMake) 검증

- `pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0` 가 정상 출력되는지
- CMakeLists.txt 에서 `find_package(PkgConfig REQUIRED)` + `pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0)` 추가 필요
- `target_link_libraries` 에 `${GST_LIBRARIES}` 추가
- `target_include_directories` 에 `${GST_INCLUDE_DIRS}` 추가

### 0.3 🟢 검증 명령 보강

NEXT_SESSION.md 현재 검증:
```bash
gst-inspect-1.0 rtspsrc / rtph264depay / h264parse / avdec_h264 / appsink / appsrc / rtspserver
```

추가 권장:
```bash
# 1. 실제 RTSP 클라이언트 동작 (네트워크 dummy 검증)
gst-launch-1.0 -v videotestsrc num-buffers=10 ! x264enc ! rtph264pay ! \
    udpsink host=127.0.0.1 port=5004
# (별 터미널에서)
gst-launch-1.0 udpsrc port=5004 caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" ! \
    rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! fakesink dump=true

# 2. avdec_h264 실제 디코드 가능 확인
gst-launch-1.0 videotestsrc num-buffers=5 ! x264enc ! decodebin ! fakesink

# 3. pkg-config 정상 (빌드 컨테이너 안에서)
pkg-config --modversion gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0
# 1.20.x 출력 기대 (Ubuntu 22.04)
```

---

## Phase 2 심층 — rtspsrc → 디코드 → AVFrame

### 2.1 🔴 Critical — reconnect 로직 누락

**문제:** [happytimesoft 의 `reconnectAfterFreeConnect`](code/Protocol/RTSP/rtsp/src/rtsp_proxy.cpp) 처럼 명시적 reconnect 가 있지만, **GStreamer rtspsrc 는 자동 reconnect 안 한다**.

rtspsrc 가 timeout / error 발생 시:
- `GstRTSPSrcTimeout` 메시지를 bus 로 발송 (RTCP timeout 시만!)
- `GST_MESSAGE_ERROR` 메시지 (네트워크 끊김 시)
- **그 후 자동으로 다시 연결하지 않음** — pipeline 이 ERROR state 로 멈춤

**우리가 해야 할 것 (GstRtspClient 책임):**
1. bus watch 등록: `gst_bus_add_signal_watch(bus)` + connect `message::error`, `message::eos`, `message::element`
2. ERROR 또는 timeout 발생 시:
   - pipeline `gst_element_set_state(pipeline, GST_STATE_NULL)`
   - 1~2초 sleep (exponential backoff: 1, 2, 4, 8, 16 sec...)
   - `gst_element_set_state(pipeline, GST_STATE_PLAYING)`
3. 무한 재시도. metric 으로 `detectbase_rtsp_reconnect_total{cam_id}` 노출.

**LOC 영향:** GstRtspClient 에 +50 LOC (reconnect 로직)
- NEXT_SESSION.md 의 Phase 3 LOC 추정 (300) 에 반영해야 함

### 2.2 🟡 rtspsrc 속성 명세 미상세

NEXT_SESSION.md 에 rtspsrc 속성 명시 없음. 권장:

```cpp
g_object_set( G_OBJECT(rtspsrc),
    "location",          url.c_str(),
    "user-id",           user.c_str(),         // 카메라 인증 (있을 시)
    "user-pw",           pass.c_str(),
    "latency",           200,                   // 200ms 버퍼링 (저지연/안정성 trade-off)
    "timeout",           2000000,               // 2초 UDP timeout 후 TCP 전환 (μs)
    "tcp-timeout",       5000000,               // 5초 TCP 연결 실패 시 에러 (μs)
    "do-retransmission", TRUE,                  // RTX (RFC 4588) 활성
    "keepalive",         TRUE,                  // RTSP keep-alive
    "protocols",         GST_RTSP_LOWER_TRANS_TCP_HTTP,  // 또는 UDP 자동선택
    "buffer-mode",       4,                     // BUFFER_MODE_AUTO (적응형)
    NULL );
```

**주의:**
- `latency=200` 은 기본값. happytimesoft 가 0 (즉시 처리) 라면 우리도 0 으로
- `protocols` — UDP first, TCP fallback. 일부 NAT 환경에서 TCP 강제 필요
- `retry` 속성은 reconnection 과 무관 (UDP 포트 할당 재시도)

### 2.3 🟡 픽셀 포맷 처리

- `avdec_h264` 출력: 기본 I420 (YUV420 planar)
- `mppvideodec` 출력 (2단계): NV12
- 현재 happytimesoft + ffmpeg 출력: 보통 I420 (또는 NV12)

**downstream ([VisionCommon/FramePreProcessor](code/VisionCommon/src/FramePreProcessor.cpp)) 입력 포맷 확인 필요:**
```bash
grep -n "AVPixelFormat\|AV_PIX_FMT" /home/claudedev/DetectBase/code/VisionCommon/src/FramePreProcessor.cpp /home/claudedev/DetectBase/code/VisionCommon/src/SwsContextManager.cpp
# 어느 포맷을 받는지 확인
```

**결론:**
- Phase 2 pipeline 에 `videoconvert` 추가하여 포맷 통일:
```
rtspsrc ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! \
    "video/x-raw,format=I420" ! appsink
```
- 2단계 mppvideodec 전환 시도 caps 만 바꾸면 됨

### 2.4 🟡 appsink 속성 — backpressure / drop 정책

```cpp
g_object_set( G_OBJECT(appsink),
    "emit-signals", TRUE,           // new-sample 시그널 발송
    "sync",         FALSE,          // 실시간 처리 (PTS 무시)
    "max-buffers",  2,              // 큐 최대 2 프레임
    "drop",         TRUE,           // 가득 시 oldest drop (백프레셔)
    NULL );

g_signal_connect( appsink, "new-sample", G_CALLBACK(OnNewSample), this );
```

- `max-buffers=2` + `drop=true` = NPU 가 못 따라가도 메모리 안 쌓임
- 현재 happytimesoft 의 `skip_if_already_exist_frame_in_q` 와 동일 정책

---

## Phase 3 심층 — Bridge (GstBuffer ↔ AVFrame)

### 3.1 🔴 Critical — AVFrame deleter 정확성

**문제:** shared_ptr<AVFrame> 가 어디서 생성되든 deleter 가 **반드시 `av_frame_free`** 여야 한다. 그렇지 않으면:
- 단순 `default_delete<AVFrame>` 만 호출 시 → AVFrame 객체 자체는 free 되지만 **data buffer (av_buffer) 는 leak**
- 1080p I420 = 3.1 MB. 30fps × 4cam = 372 MB/s leak. 10초면 OOM.

**현재 happytimesoft 측 deleter 확인 필요:**
```bash
grep -n "make_shared<AVFrame>\|shared_ptr<AVFrame>.*(" \
    /home/claudedev/DetectBase/code/Protocol/RTSP/rtsp/src/rtsp_proxy.cpp \
    /home/claudedev/DetectBase/code/Protocol/RTSP/media/src/video_decoder.cpp
```

**우리 Bridge 구현 (정확):**
```cpp
std::shared_ptr<AVFrame> CreateAVFrameFromGstBuffer( GstBuffer* buf, GstCaps* caps ) {
    AVFrame* frame = av_frame_alloc();
    if( !frame ) return nullptr;

    // 옵션 A — zero-copy (AVBufferRef + GstBuffer ref)
    GstMapInfo map;
    if( !gst_buffer_map(buf, &map, GST_MAP_READ) ) {
        av_frame_free(&frame);
        return nullptr;
    }

    // GstBuffer 의 메모리에 wrapping. GstBuffer ref/unref 가 AVBufferRef 콜백에 연결됨
    GstBuffer* buf_ref = gst_buffer_ref(buf);  // 추가 ref
    frame->buf[0] = av_buffer_create(
        map.data, map.size,
        [](void* opaque, uint8_t*) {
            GstBuffer* b = static_cast<GstBuffer*>(opaque);
            gst_buffer_unmap(b, /*map_info*/ nullptr); // 주의: map_info 저장 필요
            gst_buffer_unref(b);
        },
        buf_ref, 0);
    // ... data 포인터 / line size 설정 (포맷별)

    // shared_ptr deleter 는 av_frame_free
    return std::shared_ptr<AVFrame>(frame, [](AVFrame* f) { av_frame_free(&f); });
}
```

- 위 코드는 zero-copy 시도 — 검증 필요. GstMapInfo 의 lifetime 관리가 정확해야 함.

**옵션 B — 단순 memcpy (실패 시 fallback):**
```cpp
std::shared_ptr<AVFrame> CreateAVFrameFromGstBuffer_Copy( GstBuffer* buf ) {
    AVFrame* frame = av_frame_alloc();
    // frame->format/width/height 설정
    av_frame_get_buffer(frame, 32);  // 새 버퍼 할당

    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_READ);
    std::memcpy(frame->data[0], map.data, map.size);   // 단순 복사
    gst_buffer_unmap(buf, &map);

    return std::shared_ptr<AVFrame>(frame, [](AVFrame* f) { av_frame_free(&f); });
}
```

- 1080p I420 = 3.1 MB × 30 × 4cam = **372 MB/s** memcpy
- RK3588 메모리 대역폭 ~10 GB/s → 3.7% 사용 → 허용 가능
- zero-copy 실패 시 안전한 fallback

### 3.2 🟡 Bridge 구현 LOC 재산정

NEXT_SESSION.md / ONVIF 설계의 Phase 3 LOC = 150
**실제 추정:**
- AVFrame wrapping (zero-copy 시도 + fallback): 80
- GstCaps → AVPixelFormat 변환 함수: 30
- AVFrame data 포인터/stride 설정 (I420/NV12 각각): 30
- 에러 처리 / 로깅: 20
- 합: **160 LOC** (당초 150 → 160. 작은 차이)

### 3.3 🟡 GstRtspClient 구조 + reconnect

Phase 3 의 GstRtspClient LOC 재산정 (Phase 2.1 reconnect 반영):
- pipeline 생성 (rtspsrc/depay/parse/decode/convert/appsink): 80
- bus watch + 시그널 핸들러: 40
- **reconnect 로직 (2.1)**: 50
- appsink new-sample 콜백 → Bridge → SafeQueue: 40
- ONVIF metadata appsrc 셋업: 30
- 라이프사이클 (start/stop/cleanup): 40
- 메트릭 / 로깅: 20
- 합: **300 LOC** (당초 300 = 일치)

---

## Phase 4 심층 — 녹화 pre/post-roll

### 4.1 🔴 Critical — 표준 GStreamer 부품 없음

**문제:** GStreamer 에 "이벤트 발생 시 과거 N초 + 미래 M초" 녹화하는 **표준 부품이 없다**.

후보 옵션:

| 옵션 | 방식 | 평가 |
|---|---|---|
| **A** | RidgeRun pre-record element ([docs](https://developer.ridgerun.com/wiki/index.php/GStreamer_pre-record_element)) | 상용 (라이센스 비용) |
| **B** | `queue2 max-size-time=N`(ring) + 동적 pipeline 조작으로 dump | 표준 부품, 복잡 |
| **C** | 자체 GStreamer 엘리먼트 작성 (C, GstElement 상속) | 매우 복잡 |
| **D** | C++ 라이브 ring buffer (DTLS bypass) — 우리 코드에서 H264 packet 저장하다가 트리거 시 file write | 가장 직접적 |

### 4.2 옵션 D 권장 (자체 ring buffer)

```
rtspsrc → rtph264depay → tee ─┬─→ [기존 분기: h264parse → avdec → ...]
                                │
                                └─→ appsink (raw H264 packet)
                                              ↓
                                       C++ RingBuffer (N seconds, drop oldest)
                                              ↓
                                    [트리거 발생 시]
                                              ↓
                                  ring 내용 → mp4mux 파일 → post-roll M초 추가
                                              ↓
                                       파일 닫음
```

**구현 요점:**
- ring buffer = `std::deque<H264Packet>` + max-size-time 정책
- H264Packet = `{ uint8_t* data, size_t size, uint64_t pts, bool is_keyframe }`
- 파일 쓰기 = ffmpeg libavformat API 직접 사용 (`avformat_alloc_output_context2` + `avformat_write_header` + `av_write_frame` + `av_write_trailer`)
- post-roll: 트리거 후 M초 동안 ring 에 들어오는 packet 도 같이 파일에 write

**LOC 재산정:**
- 당초 400 LOC 추정
- ring buffer: 100
- mp4 mux (libavformat): 150
- 트리거 처리 + post-roll: 80
- 인터페이스 (StartRecord / StopRecord): 50
- 메트릭 / 로깅: 30
- 합: **410 LOC** (당초 400 → 410. 거의 일치)

### 4.3 🟡 GStreamer pipeline 영향

ring buffer 분기 추가로 tee + queue + appsink 3 엘리먼트 추가 → CPU 영향 미미 (raw packet 만 통과).

---

## Phase 5 심층 — gst-rtsp-server + 4cam

### 5.1 🔴 Critical — 인증 누락

**문제:** happytimesoft 는 admin/admin + user/123456 두 계정 사용 ([RtspHandler.cpp:363,370](code/Management/worker/src/RtspHandler.cpp#L363)). NEXT_SESSION.md 에 인증 명시 0.

**보강:**
```cpp
GstRTSPAuth* auth = gst_rtsp_auth_new();

// admin (모든 권한)
GstRTSPToken* admin_token = gst_rtsp_token_new(
    GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "admin", NULL );
gchar* basic_admin = gst_rtsp_auth_make_basic( "admin", "admin" );
gst_rtsp_auth_add_basic( auth, basic_admin, admin_token );
g_free( basic_admin );

// user (view only)
GstRTSPToken* user_token = gst_rtsp_token_new(
    GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "user", NULL );
gchar* basic_user = gst_rtsp_auth_make_basic( "user", "123456" );
gst_rtsp_auth_add_basic( auth, basic_user, user_token );
g_free( basic_user );

gst_rtsp_server_set_auth( server, auth );
```

**factory permissions:**
```cpp
GstRTSPPermissions* perms = gst_rtsp_permissions_new();
gst_rtsp_permissions_add_permission_for_role( perms, "admin", GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, TRUE );
gst_rtsp_permissions_add_permission_for_role( perms, "admin", GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, TRUE );
gst_rtsp_permissions_add_permission_for_role( perms, "user", GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, TRUE );
gst_rtsp_permissions_add_permission_for_role( perms, "user", GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, TRUE );
gst_rtsp_media_factory_set_permissions( factory, perms );
```

**LOC 영향:** GstRtspServer 에 +30 LOC

### 5.2 🟡 per-camera mount path 설계

```
rtsp://server:8554/cam1
rtsp://server:8554/cam2
rtsp://server:8554/cam3
rtsp://server:8554/cam4
```

각 cam 별로 별도 `GstRTSPMediaFactory`:
```cpp
for( int id = 0; id < 4; ++id ) {
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch( factory, launch_pipeline_for_cam(id) );
    gst_rtsp_media_factory_set_shared( factory, TRUE );  // 다중 클라이언트 동일 stream
    gst_rtsp_mount_points_add_factory( mounts, ("/cam" + std::to_string(id)).c_str(), factory );
}
```

**중요:** `set_shared(TRUE)` — 같은 mount path 에 동시 연결한 클라이언트들이 **같은 pipeline 공유**. CPU/메모리 절감. 기본값은 FALSE 라 명시 필요.

### 5.3 🟡 ONVIF service mount — video + metadata 동시

ONVIF 호환 클라이언트가 SDP 로 video + metadata 두 stream 받으려면 **하나의 mount path 에 두 stream 노출**:

```cpp
const char* launch =
    "( "
    "  appsrc name=videosrc is-live=true ! rtph264pay name=pay0 pt=96 "
    "  appsrc name=metasrc  is-live=true ! "
    "      application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA,clock-rate=90000,payload=97 ! "
    "      identity name=pay1 "  // pay%d 명명 규칙 트릭
    ")";
gst_rtsp_media_factory_set_launch( factory, launch );
```

- `name=pay0`, `name=pay1` 으로 stream 인덱스 부여
- ONVIF 검증 (PoC Phase 1 Day 0 의 Critical 13.1) 통과 시 이 방식 동작 확정

### 5.4 🔴 Critical — Shutdown 순서

**문제:** NEXT_SESSION.md 에 shutdown 순서 명시 0. GStreamer 는 잘못된 순서로 종료하면 hang 또는 use-after-free 가능.

**정확한 순서 (CLAUDE.md 의 `// !!! DO NOT REORDER !!!` 정책 따름):**

```cpp
void GracefulShutdown()
{
    // !!! DO NOT REORDER !!!

    // 1. 새 클라이언트 거부 (RTSP server 가 새 SETUP 받지 않게)
    gst_rtsp_server_remove_signal_handlers( server_ );

    // 2. 메타데이터 송신 정지 (모든 cam 의 페이로더)
    for( auto& [id, client] : clients_ ) {
        client->StopMetadataPush();
    }

    // 3. 감지 스레드 정지 (RtspDetectorUnit 의 InferenceThread)
    for( auto& [id, unit] : units_ ) {
        unit->StopDetection();
    }

    // 4. 큐 terminate (SafeQueue<AVFrame> 가 nullopt 반환하게)
    for( auto& [id, q] : avframe_queues_ ) {
        q->terminate();
    }

    // 5. 현재 연결된 RTSP 클라이언트 disconnect
    GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool( server_ );
    gst_rtsp_session_pool_filter( pool, /*remove all*/ nullptr, nullptr );
    g_object_unref( pool );

    // 6. mount points 제거
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points( server_ );
    for( int id = 0; id < 4; ++id ) {
        gst_rtsp_mount_points_remove_factory( mounts, ("/cam" + std::to_string(id)).c_str() );
    }
    g_object_unref( mounts );

    // 7. RTSP 클라이언트 pipeline (rtspsrc 분기) NULL state
    for( auto& [id, client] : clients_ ) {
        client->SetPipelineNull();
    }

    // 8. GMainLoop quit
    g_main_loop_quit( main_loop_ );

    // 9. GMainLoop 스레드 join
    if( main_loop_thread_.joinable() ) {
        main_loop_thread_.join();
    }

    // 10. server detach + free
    g_object_unref( server_ );
}
```

**LOC 영향:** RtspHandler 재작성 에 +60 LOC (당초 200 → 260)

### 5.5 🟡 Phase 5 LOC 재산정

당초 +400 LOC (Phase 4 녹화 포함)
- gst-rtsp-server (5.1 인증 + 5.2 factory + 5.3 ONVIF + 5.4 shutdown): **+250 LOC**
- 4cam parity glue: +50 LOC
- 합: **300 LOC** (Phase 5 자체)

---

## Phase 6 심층 — 48h soak test

### 6.1 🟡 비교 baseline 확정 필요

**문제:** "happytimesoft 대비 비교" 라고만 적힘. 어떤 항목을, 어떤 데이터로?

**보강 — baseline 데이터 소스:**

| 항목 | happytimesoft baseline | GStreamer 측정 |
|---|---|---|
| RSS 추세 | 이번 W-14 + jemalloc 48h 테스트 데이터 | Phase 6 48h snap_*.txt |
| DFPS | 53~54 (현재 metric) | GStreamer 48h 평균 |
| 디코드 CPU | top -p PID 측정 (현재 ~20%) | 같은 방법 |
| 메모리 RssAnon | snap 의 RssAnon 컬럼 | 같음 |
| 스레드 수 | 37 | GStreamer 측 (Bridge thread 등 추가 가능) |
| FD 수 | snap 의 fd_count | 같음 |
| 디스크 cleanup 빈도 | half_files 카운트 | 같음 |
| 메타데이터 packet drop | (현재 메트릭 없음) | onvif_metadata_drops_total |
| reconnect 빈도 | (현재 메트릭 없음) | rtsp_reconnect_total |
| latency (e2e) | (측정 안 됨) | rtspsrc latency=200 → ~200ms |

### 6.2 🟡 회귀 검출 기준

| 결과 | 기준 |
|---|---|
| **PASS** | 모든 메트릭이 happytimesoft 의 ±10% 이내, leak 없음 |
| **WARN** | CPU/메모리가 +10~30% 증가, leak 없음 (튜닝 후 진행 가능) |
| **FAIL** | leak 검출 / DFPS 50 미만 / reconnect 빈발 / RSS 추세 우상향 |

### 6.3 🟢 ASan 빌드 사전 검증 권장

48h test 본 검증 전, **6h ASan 단축 테스트** 권장:
- 새로 작성된 코드 (페이로더 240 + Bridge 160 + Client 300 + Recorder 410 + Server 300 = **1,410 LOC**) 에 leak 가능성 큼
- 6h ASan → 발견 leak top 3 수정 → 48h 정식 테스트

**Phase 6 일정 보강:**
- Day 1: ASan 빌드 + 6h 단축 테스트
- Day 2: leak 수정 + 재빌드
- Day 3~7: 48h 정식 soak test
- Day 8: 결과 분석 + 비교 리포트

---

## 종합 영향 정리

### LOC 재산정 (모든 보강 반영)

| Phase | NEXT_SESSION.md 기존 | 보강 후 | 차이 |
|---|---|---|---|
| 1 (페이로더) | 200 | 248 | +48 |
| 3 (Bridge) | 150 | 160 | +10 |
| 3 (Client+reconnect) | 300 | 300 | 0 (이미 포함) |
| 4 (Recorder) | 400 | 410 | +10 |
| 5 (Server+auth+shutdown) | 250 | 300 | +50 |
| **합계 신규** | 1,300 | **1,418** | **+118** |
| 변경 (Consumer) | ~50 | ~50 | 0 |

### Critical issue 정리

| # | 위치 | 해결 |
|---|---|---|
| C1 | ONVIF appsrc → factory 통합 (§13.1) | PoC Phase 1 Day 0 검증 |
| **C2** | **AVFrame deleter (av_frame_free)** | **Phase 3 구현 시 명시** |
| **C3** | **rtspsrc reconnect 로직** | **Phase 2/3 구현 시 명시 (+50 LOC)** |
| **C4** | **녹화 pre/post-roll 표준 부품 없음** | **옵션 D 자체 ring buffer + libavformat** |
| **C5** | **인증 (admin/admin, user/123456)** | **Phase 5 +30 LOC** |
| **C6** | **Shutdown 순서 미정의** | **Phase 5 +60 LOC, 10단계 명시** |

### 일정 영향

NEXT_SESSION.md 기존:
- Phase 1 = 2.5일 (§13 반영)
- Phase 2~5 = 10일
- Phase 6 = 7일
- 합: 19.5일

보강 후:
- Phase 1 = 2.5일 (그대로)
- Phase 2 = 2일 (rtspsrc 속성/픽셀 포맷 명시 — 추가 시간 없음)
- Phase 3 = 3.5일 (당초 3일 + reconnect/Bridge 구체화 +0.5일)
- Phase 4 = 2.5일 (당초 2일 + 자체 ring buffer 복잡도 +0.5일)
- Phase 5 = 3.5일 (당초 3일 + 인증/shutdown 명시 +0.5일)
- Phase 6 = 8일 (당초 7일 + ASan 사전 1일)
- 합: **22일** (+2.5일)

전체 PoC: 3주 → **3.5주** (여유 1주 포함 시 안전)

---

## 권장 조치

1. **NEXT_SESSION.md 보강**:
   - Phase 0 의 apt 패키지 목록 갱신
   - 각 Phase 에 이 문서 참조 추가
   - Phase 3/4/5 LOC/일정 재산정 반영

2. **Critical 6개 항목**을 PoC 진입 직전 체크리스트로 명시 (실제 코드 작성 시 누락 방지)

3. **C2~C6 은 PoC 진입 후 즉시 검증** 가능. C1 (ONVIF appsrc 통합) 만 PoC Phase 1 Day 0 에 사전 검증 필수.

## 결론

NEXT_SESSION.md 의 GStreamer Phase 0~6 큰 줄기는 **맞다**. 다만:

- **신규 Critical 5개 (C2~C6)** 발견 — 구현 단계에서 누락하면 leak/UAF/통합 실패
- 일정 +2.5일 (3주 → 3.5주)
- LOC +118 (1,300 → 1,418)

모든 Critical 항목은 **해결 방안 명시됨**. PoC 진입 가능 상태 유지.
