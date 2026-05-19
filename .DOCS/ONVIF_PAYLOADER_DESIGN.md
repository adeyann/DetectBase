# ONVIF Metadata 페이로더 자체 구현 설계

> **STATUS (2026-05-15)**: 본 설계는 GStreamer 통합 시도 (롤백) 시점의 사전 설계 자료.
> 현재 happytimesoft baseline 복원 상태에서는 happytimesoft 라이브러리의 자체 ONVIF metadata 출력 메커니즘을 그대로 사용. GStreamer 재작업 branch 에서 재참조.

---

**작성일**: 2026-05-13
**대상**: PoC Phase 1 사전 설계 (실제 코드 작성은 48h 테스트 + 후처리 후)
**목표 LOC**: 약 240 줄 (당초 200 추정 → 정밀 산정 240)

## 1. 배경 — 왜 자체 구현인가

- ONVIF metadata RTP 송신용 GStreamer 부품 (`rtponvifmetadatapay`) 은 `gst-plugins-rs` (Rust) 에 있음
- 우분투 22.04 apt 공식 저장소에 없음 → Rust 빌드체인 도입 회피 위해 자체 구현 선택 (결정 1)
- ONVIF metadata RTP 페이로드 형식 단순 → 자체 구현 위험 낮음

## 2. ONVIF Streaming Spec 핵심 규칙 (정확한 사실)

[ONVIF Streaming Spec v25.12](https://www.onvif.org/specs/stream/ONVIF-Streaming-Spec.pdf) 기준.

| 규칙 | 값 |
|---|---|
| RTP payload type | dynamic 96~127 (보통 96) |
| Clock rate | **90,000 Hz** (RTP video 표준) |
| Encoding name | `VND.ONVIF.METADATA` |
| RTP media type | `application` |
| MIME (GStreamer caps) | `application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA,clock-rate=90000,payload=96` |
| **Marker bit = 1** | **XML document의 마지막 RTP 패킷에 표시 = "문서 끝"** |
| 페이로드 본체 | XML (`<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema">...`) |
| 크기 제한 | **없음** (MTU 초과 시 fragmentation) |
| 압축 | GZIP optional (RFC 1952 헤더) — **우리는 사용 안 함** |
| 동기화 권장 | 최대 1초마다 새 XML document 시작 |

## 3. 현재 코드와의 인터페이스 (보존)

[RtspDetectorUnit.cpp:480](code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L480) `ConvertTrackingBoxesToMetaData()` 는 **완결된 XML document 한 개**를 생성:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema">
  <tt:VideoAnalytics>
    <tt:Frame UtcTime="2026-05-13T11:30:00.123">
      <tt:Object ObjectId="0">
        <tt:Appearance>
          <tt:Shape>
            <tt:BoundingBox left="..." top="..." right="..." bottom="..."/>
            <tt:TrackId trackid="..."/>
            <tt:CenterOfGravity x="60.0" y="50.0"/>
          </tt:Shape>
          <tt:Class>
            <tt:ClassCandidate>
              <tt:Type>MaybeUnused</tt:Type>
              <tt:Likelihood>0.97</tt:Likelihood>
            </tt:ClassCandidate>
          </tt:Class>
        </tt:Appearance>
      </tt:Object>
      ...
    </tt:Frame>
  </tt:VideoAnalytics>
</tt:MetadataStream>
```

호출처: [RtspDetectorUnit.cpp:1164](code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1164)
```cpp
SendDetectResultToMetaData( entire_track_results );
```

→ **ConvertTrackingBoxesToMetaData() 100% 보존**. `SendDetectResultToMetaData()` 안의 한 줄만 교체:

```cpp
// AS-IS (happytimesoft)
proxy_ptr_->runCallBacks( data, size, DATA_TYPE_METADATA );

// TO-BE (GStreamer)
gst_client_->PushMetadataXml( meta_data );
```

## 4. 파이프라인 설계

per-camera RTSP 출력 파이프라인에 metadata stream 추가:

```
[video 분기 — 기존]
appsrc(decoded H264) ! rtph264pay ! ─┐
                                      ├─→ rtspserver (mount point)
[metadata 분기 — 신규]                 │
appsrc(RTP packets) !                ─┘
   application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA,
                       clock-rate=90000,payload=96
```

**핵심 결정: 우리 페이로더 출력은 이미 RTP 패킷이다.**
- 즉 `appsrc` 가 받을 buffer = 완성된 RTP packet (header 12B + payload)
- GStreamer 의 `rtphdrextpay` 같은 추가 페이로딩 없음
- `application/x-rtp` caps 만 붙여서 rtspserver 로

## 5. 클래스 설계

### 5.1 `ONVIFMetadataPayloader` (자체 구현 핵심 클래스)

위치: `code/Protocol/RTSP_GST/onvif_payloader.{h,cpp}` (신규 디렉토리)

```cpp
namespace MGEN
{
    /**
     * @brief ONVIF metadata RTP 페이로더.
     *
     * XML document 1개 → N개 RTP packet (마지막 marker=1) 로 변환 후
     * GstAppSrc 로 push.
     *
     * @note 한 PushXml() 호출 = 한 XML document = 마지막 패킷의 RTP marker bit = 1.
     *       페이로드 크기가 MTU 초과 시 자동 fragmentation.
     *
     * @note Thread-safe: SafeQueue 통해 입력 직렬화. 송신 스레드 1개.
     */
    class ONVIFMetadataPayloader
    {
    public:
        struct Config {
            uint32_t ssrc          = 0;       ///< 0 = random 생성
            uint8_t  payload_type  = 96;      ///< 96~127 dynamic
            size_t   mtu           = 1400;    ///< RTP 페이로드 최대 크기 (이더넷 MTU 1500 - IP/UDP 헤더)
        };

        /**
         * @param appsrc 외부 소유 (GstRtspClient). weak ref.
         * @param cfg    선택. 기본값 사용 가능.
         */
        explicit ONVIFMetadataPayloader( GstAppSrc* appsrc, const Config& cfg = {} ) noexcept;
        ~ONVIFMetadataPayloader();

        // 복사/이동 금지 (CLAUDE.md 정책)
        ONVIFMetadataPayloader( const ONVIFMetadataPayloader& )            = delete;
        ONVIFMetadataPayloader& operator=( const ONVIFMetadataPayloader& ) = delete;
        ONVIFMetadataPayloader( ONVIFMetadataPayloader&& )                 = delete;
        ONVIFMetadataPayloader& operator=( ONVIFMetadataPayloader&& )      = delete;

        /**
         * @brief XML document 한 개를 RTP 패킷 시퀀스로 변환하여 appsrc 에 push.
         *
         * - 크기 ≤ MTU: 단일 packet, marker = 1
         * - 크기 > MTU: N개 packet, 마지막만 marker = 1
         *
         * @return true: push 성공 / false: appsrc 가 NULL 이거나 push 실패
         */
        bool PushXml( const std::string& xml_document ) noexcept;

        // 통계 (Prometheus 메트릭 별도 노출)
        uint64_t GetPacketCount() const noexcept { return packet_count_.load(); }
        uint64_t GetByteCount()   const noexcept { return byte_count_.load();   }
        uint64_t GetDropCount()   const noexcept { return drop_count_.load();   }

    private:
        // RTP 헤더 12 byte 빌드 (network byte order)
        void WriteRtpHeader( uint8_t* buf, bool marker, uint32_t timestamp ) noexcept;

        // 단일 RTP packet appsrc push
        bool PushPacket( const uint8_t* payload, size_t size, bool marker, uint32_t timestamp ) noexcept;

        // 90kHz timestamp 현재값
        uint32_t Now90kHz() const noexcept;

        GstAppSrc*                    appsrc_;        ///< weak ref. owner: GstRtspClient
        Config                        cfg_;
        uint32_t                      ssrc_;
        std::atomic<uint16_t>         seq_num_;       ///< 시작 random
        std::chrono::steady_clock::time_point base_time_;

        std::atomic<uint64_t>         packet_count_ { 0 };
        std::atomic<uint64_t>         byte_count_   { 0 };
        std::atomic<uint64_t>         drop_count_   { 0 };
    };
}
```

### 5.2 RTP 헤더 (RFC 3550) — 12 bytes

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
```

| 필드 | 값 | 비트 |
|---|---|---|
| V (version) | 2 | 2 |
| P (padding) | 0 | 1 |
| X (extension) | 0 | 1 |
| CC (CSRC count) | 0 | 4 |
| **M (marker)** | **0 또는 1** (XML 끝) | 1 |
| PT (payload type) | 96 | 7 |
| Sequence number | 각 패킷마다 +1 | 16 |
| Timestamp | 90kHz 기준 | 32 |
| SSRC | 스트림 식별자 | 32 |

### 5.3 Fragmentation 로직

```cpp
bool ONVIFMetadataPayloader::PushXml( const std::string& xml ) noexcept
{
    if( !appsrc_ ) return false;

    const size_t   payload_max = cfg_.mtu - 12;   // RTP header
    const uint32_t ts          = Now90kHz();      // 같은 XML document 안에서는 동일 timestamp
    const size_t   total       = xml.size();

    const uint8_t* data        = reinterpret_cast<const uint8_t*>( xml.data() );
    size_t         remaining   = total;

    while( remaining > 0 ) {
        const size_t chunk  = std::min( remaining, payload_max );
        const bool   marker = ( chunk == remaining );   // 마지막 조각만 marker = 1

        if( !PushPacket( data, chunk, marker, ts ) ) {
            drop_count_.fetch_add( 1 );
            return false;
        }
        data      += chunk;
        remaining -= chunk;
    }
    return true;
}
```

### 5.4 GStreamer 연동 (appsrc push)

```cpp
bool ONVIFMetadataPayloader::PushPacket( const uint8_t* payload, size_t size, bool marker, uint32_t ts ) noexcept
{
    const size_t total_size = 12 + size;
    GstBuffer*   buf = gst_buffer_new_allocate( nullptr, total_size, nullptr );
    if( !buf ) return false;

    GstMapInfo info;
    if( !gst_buffer_map( buf, &info, GST_MAP_WRITE ) ) {
        gst_buffer_unref( buf );
        return false;
    }

    WriteRtpHeader( info.data, marker, ts );
    std::memcpy( info.data + 12, payload, size );

    gst_buffer_unmap( buf, &info );

    GstFlowReturn flow = gst_app_src_push_buffer( appsrc_, buf );  // buffer 소유권 양도
    if( flow != GST_FLOW_OK ) {
        return false;
    }

    packet_count_.fetch_add( 1 );
    byte_count_.fetch_add( total_size );
    return true;
}
```

### 5.5 RTP Header 빌드

```cpp
void ONVIFMetadataPayloader::WriteRtpHeader( uint8_t* buf, bool marker, uint32_t ts ) noexcept
{
    const uint16_t seq = seq_num_.fetch_add( 1 );

    buf[0] = 0x80;                                   // V=2, P=0, X=0, CC=0
    buf[1] = ( marker ? 0x80 : 0x00 ) | cfg_.payload_type;
    buf[2] = static_cast<uint8_t>( seq >> 8 );
    buf[3] = static_cast<uint8_t>( seq & 0xFF );

    // timestamp (big-endian)
    buf[4] = static_cast<uint8_t>( ts >> 24 );
    buf[5] = static_cast<uint8_t>( ts >> 16 );
    buf[6] = static_cast<uint8_t>( ts >>  8 );
    buf[7] = static_cast<uint8_t>( ts & 0xFF );

    // SSRC (big-endian)
    buf[8]  = static_cast<uint8_t>( ssrc_ >> 24 );
    buf[9]  = static_cast<uint8_t>( ssrc_ >> 16 );
    buf[10] = static_cast<uint8_t>( ssrc_ >>  8 );
    buf[11] = static_cast<uint8_t>( ssrc_ & 0xFF );
}
```

### 5.6 Timestamp 계산 (90 kHz)

```cpp
uint32_t ONVIFMetadataPayloader::Now90kHz() const noexcept
{
    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>( now - base_time_ ).count();
    // 90 kHz = 1 tick / (1/90000 sec) = 1 tick per 11,111 ns
    return static_cast<uint32_t>( ( elapsed * 90 ) / 1000000 );  // ns × 90 / 1e6 = 90kHz ticks
}
```

## 6. 통합 흐름 (RtspDetectorUnit → GstRtspClient → 페이로더)

```
[NPU 추론 결과 발생]
        │
        ▼
RtspDetectorUnit::SendDetectResultToMetaData( results )
        │
        │  ConvertTrackingBoxesToMetaData()  ← 100% 기존 코드 유지
        │
        ▼
gst_client_->PushMetadataXml( xml_string )
        │
        ▼
ONVIFMetadataPayloader::PushXml( xml_string )
        │
        │  for( chunk in fragments ):
        │      PushPacket( chunk, marker=last?, ts )
        │
        ▼
GstAppSrc → application/x-rtp → rtspserver → 클라이언트(VLC 등)
```

## 7. BasicLibs 활용

| 도구 | 사용처 |
|---|---|
| `SafeQueue<std::string>` | (선택) appsrc 가 backpressure 신호 보낼 때 — Phase 1 에서는 미사용, 단순 직접 push |
| `MgenLogger` | `MLOG_DEBUG`, `MLOG_ERROR` (push 실패 시) |
| `MetricsRegistry` | `detectbase_onvif_metadata_packets_total`, `detectbase_onvif_metadata_bytes_total`, `detectbase_onvif_metadata_drops_total` |
| `math_utils` | timestamp 변환 (불필요. inline 함수면 충분) |
| `UUIDGenerator` | SSRC 랜덤 생성 (선택) — `std::random_device` 로도 충분 |

→ 핵심은 **MgenLogger + MetricsRegistry** 만. SafeQueue/SafeThread 는 단순 동기 push 라 미사용.

## 8. LOC 분해 (정밀)

| 구성 | LOC | 비고 |
|---|---|---|
| 헤더 (`onvif_payloader.h`) | 85 | Doxygen 포함 |
| Constructor / Destructor / SSRC init | 25 | random SSRC, base_time |
| `WriteRtpHeader()` | 22 | 12byte 빌드 |
| `Now90kHz()` | 8 | inline 가능 |
| `PushPacket()` | 32 | GstBuffer 할당+map+push |
| `PushXml()` (fragmentation) | 28 | while loop |
| 메트릭 등록 (constructor 일부) | 18 | 3개 카운터 |
| 노이즈 (include, namespace 등) | 22 | |
| **합계** | **240** | |

## 9. 검증 시나리오 (Phase 1 의 2일차)

### 9.1 단위 테스트 (자체)
| 케이스 | 입력 | 기대 |
|---|---|---|
| 단일 작은 XML | 500 bytes | 1 packet, marker=1, seq=N |
| MTU 경계 | 1388 bytes (=1400-12) | 1 packet, marker=1 |
| MTU 초과 | 3000 bytes | 3 packet (1388+1388+224), 마지막만 marker=1, seq=N,N+1,N+2 |
| 큰 XML | 10 KB | 8 packet, 마지막 marker=1 |
| 연속 호출 100회 | — | seq 정상 증가, timestamp 단조 증가, SSRC 동일 |
| appsrc=NULL | — | false 반환, drop_count+=1 |

### 9.2 통합 검증 (gst-launch 명령줄)
```bash
gst-launch-1.0 \
    rtspsrc location=rtsp://localhost:8554/cam1 ! \
    rtpdec ! application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA ! \
    fakesink dump=true
# stdout 에 RTP 패킷 hex dump → marker bit 확인, XML 본체 확인
```

### 9.3 ONVIF 호환성 (VLC)
- VLC 4.x 에서 RTSP 연결
- **"Tools → Codec Information"** 에서 metadata stream 표시되는지
- 또는 **ONVIF Device Manager (Windows)** 로 metadata stream 수신 확인

### 9.4 트래픽 분석 (Wireshark)
- 4cam × 10fps detect = 40 metadata/sec
- 1cam 당 추론 결과 0~5 object → XML 1~3 KB → RTP packet 1~3개
- 트래픽 추정: 4cam × 30 packet/sec × 1.5KB = 약 180 KB/sec (1.4 Mbps)

## 10. 위험 요소 / mitigation

| 위험 | 가능성 | mitigation |
|---|---|---|
| appsrc backpressure (큐 가득참) | 낮음 (메타 트래픽 작음) | drop_count 메트릭으로 관측, 발생 시 SafeQueue 도입 |
| RTP timestamp jitter | 매우 낮음 | 90kHz steady_clock 기반, monotonic |
| seq_num 16bit overflow | 매우 낮음 | uint16_t 자연 wrap, 정상 RTP 동작 |
| SSRC 충돌 | 매우 낮음 | random 32bit, 4cam 내 collision 확률 ~0 |
| XML 손상 (zero-byte injection) | 낮음 | ConvertTrackingBoxesToMetaData 가 안전한 XML 생성 |
| ONVIF spec 해석 오류 | 중간 | VLC + ODM 두 클라이언트로 호환성 cross-check |

## 11. 다음 단계 (실제 코드 작성 시)

이 설계 그대로 진행:

1. **Phase 1 Day 1**: `code/Protocol/RTSP_GST/onvif_payloader.h/cpp` 작성 (이 설계 그대로). 단위 테스트 작성. `gst-launch` 로 hex dump 검증.

2. **Phase 1 Day 2**: VLC + ONVIF Device Manager 로 호환성 검증. 트래픽 측정. 메트릭 검증.

3. **Phase 1 종료 조건**:
   - 단위 테스트 6/6 통과
   - VLC 가 metadata stream 인식
   - 30분 연속 송신 후 drop_count = 0
   - 메트릭 3종 (packets/bytes/drops) 노출

## 12. 결론

- 자체 구현 **240 LOC**, 위험도 매우 낮음
- ConvertTrackingBoxesToMetaData() 등 기존 코드 100% 보존 — 인터페이스 1줄만 교체
- ONVIF Streaming Spec 정확히 반영 (marker bit, 90kHz, payload type)
- BasicLibs 도구 활용 (MgenLogger, MetricsRegistry)
- VLC + ODM 두 클라이언트로 cross-check 검증

## 13. 심층 재검토 보강 (2026-05-13 추가)

이 절은 §1~§12 설계를 비판적으로 재검토한 결과 발견된 **Critical issue 1건 + 누락/보강 6건** 을 정리한다.

### 13.1 🔴 Critical — gst-rtsp-server 통합 방식 검증 필요

**문제:**
`gst-rtsp-server` 표준 패턴은 factory 의 bin 안에 `pay%d` 이름의 RTP payloader 엘리먼트가 있어야 stream 으로 노출됨 ([rtsp-media-factory docs](https://gstreamer.freedesktop.org/documentation/gst-rtsp-server/rtsp-media-factory.html)).

내 설계는 **이미 RTP packet 으로 만들어 appsrc 에 push**하는 방식 — 공식 예제 `test-appsrc.c` / `test-appsrc2.c` 둘 다 raw media 를 push 하고 `rtph264pay` 가 payload 하는 패턴이라 우리 방식이 **검증되지 않은 영역**.

**해법 옵션:**

| 옵션 | 방식 | 복잡도 | 위험 |
|---|---|---|---|
| **A** | appsrc 를 `name=pay2` 로 등록 + caps `application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA,clock-rate=90000,payload=96` | 낮음 | **검증 필요** |
| **B** | `GstBaseRTPPayload` 서브클래스로 GStreamer 엘리먼트 작성 (C 코드 +200 LOC) | 매우 높음 | 낮음 |
| **C** | appsrc + identity + 이름 부여 트릭 | 중간 | 검증 필요 |

**대응:**
- PoC Phase 1 에 **Day 0 (0.5일) 통합 검증 사전 단계** 추가
- 옵션 A 가 동작하는지 우선 `gst-launch-1.0` 으로 검증
- 안 되면 결정 1 재방문 (gst-plugins-rs source build 로 전환, 일정 +3일, 이미지 +500MB)

**검증 명령:**
```bash
# 1. appsrc → application/x-rtp → rtpdec 라운드트립 (자체)
gst-launch-1.0 \
    appsrc name=src caps="application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA,clock-rate=90000,payload=96" ! \
    rtpdec ! \
    fakesink dump=true

# 2. test-launch 로 RTSP factory 에 등록 가능한지
gst-launch-1.0 -v \
    rtspsrc location=rtsp://localhost:8554/test latency=0 ! \
    application/x-rtp,media=application,encoding-name=VND.ONVIF.METADATA ! \
    fakesink dump=true
```

### 13.2 🟡 Constructor 초기화 명시 (필수)

§5.1 의 클래스 다이어그램에 멤버는 있지만 constructor 본체가 비어 있어 **base_time_ / SSRC / seq_num 초기화 누락**.

```cpp
ONVIFMetadataPayloader::ONVIFMetadataPayloader( GstAppSrc* appsrc, const Config& cfg ) noexcept
    : appsrc_     ( appsrc )
    , cfg_        ( cfg )
    , base_time_  ( std::chrono::steady_clock::now() )   // ★ 누락됐던 항목
{
    std::random_device rd;
    std::mt19937 gen( rd() );

    // SSRC : 0이면 random 생성 (RFC 3550 §3 권장)
    ssrc_ = ( cfg.ssrc != 0 ) ? cfg.ssrc
                              : std::uniform_int_distribution<uint32_t>()( gen );

    // seq_num : random 16bit 시작 (RFC 3550 §3, prediction attack 방어)
    seq_num_.store( std::uniform_int_distribution<uint16_t>()( gen ) );

    // 메트릭 등록 (이름 기반 API)
    auto& reg = MetricsRegistry::Instance();
    reg.RegisterCounter( "detectbase_onvif_metadata_packets_total", "ONVIF metadata RTP packets sent" );
    reg.RegisterCounter( "detectbase_onvif_metadata_bytes_total",   "ONVIF metadata RTP bytes sent" );
    reg.RegisterCounter( "detectbase_onvif_metadata_drops_total",   "ONVIF metadata push failures" );
}
```

### 13.3 🟡 MetricsRegistry API — 이름 기반 + singleton

§5.1 설계에서 `prometheus::Counter*` 멤버 직접 보유는 **잘못**.

실제 [BasicLibs/core/metrics/MetricsRegistry.cpp](code/BasicLibs/core/metrics/MetricsRegistry.cpp) API:
- `MetricsRegistry::Instance()` — singleton
- `RegisterCounter( name, help )` — 이름 기반 등록
- `IncrementCounter( name, value )` — 이름 lookup 후 증가

**수정:** Counter*/Gauge* 멤버 제거. PushPacket 안에서 직접 호출:
```cpp
MetricsRegistry::Instance().IncrementCounter( "detectbase_onvif_metadata_packets_total", 1 );
MetricsRegistry::Instance().IncrementCounter( "detectbase_onvif_metadata_bytes_total",   total_size );
```

이름 lookup overhead 는 hashmap → ns 단위, 4cam × 30 packet/s = 120/s 에서 무시 가능.

### 13.4 🟡 appsrc 속성 — PTS / live / format

§5.4 의 `PushPacket()` 에서 GstBuffer PTS 미설정. 해결: **pipeline 셋업 시 appsrc 속성으로 위임**:

```cpp
// GstRtspClient 가 appsrc 생성 시:
g_object_set( appsrc,
    "is-live",       TRUE,
    "do-timestamp",  TRUE,    // GStreamer 가 자동으로 PTS 부여
    "format",        GST_FORMAT_TIME,
    "block",         FALSE,   // 큐 가득 시 drop (backpressure)
    "max-bytes",     1048576, // 1MB 한계 (backpressure)
    NULL );
```

→ 우리 페이로더는 **RTP timestamp (90kHz)** 만 책임지고, **GStreamer PTS (ns)** 는 appsrc 가 자동 부여. 두 시스템이 분리되어 깔끔.

### 13.5 🟡 RTCP / NTP-RTP mapping — GStreamer 가 자동

ONVIF 가 metadata ↔ video 매칭하려면 RTCP Sender Report 의 NTP-RTP timestamp 매핑 필요 ([Axis ONVIF Replay docs](https://developer.axis.com/vapix/network-video/onvif-replay-extension/)).

**우리가 직접 송신할 필요 없음:**
- GStreamer `rtpsession` 엘리먼트가 자동 송신 ([rtpsession docs](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpsession.html))
- gst-rtsp-server 의 RTSPMedia 가 자동으로 rtpsession 추가

**검증 필요 (PoC Phase 1):**
- Wireshark 로 RTCP SR 패킷 캡처
- NTP timestamp 와 우리 RTP timestamp (90kHz) 의 매핑이 합리적인지 확인
- video stream 의 RTCP SR 과 같은 wallclock 기준인지 확인 (그래야 사용자가 metadata↔video 매칭 가능)

### 13.6 🟡 UTF-8 boundary — 미래 안전성

§5.3 fragmentation 로직은 byte 단위로 자르므로 **UTF-8 multi-byte 문자 중간 절단 위험**.

**현재 영향:** [RtspDetectorUnit.cpp:480](code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L480) 의 XML 은 ASCII 만 사용 → 안전.

**미래 위험:** `tt:Class > tt:Type` 에 한글 (예: "사람") 등 multi-byte 문자 들어가면 위험.

**방어적 보강 (선택):**
```cpp
// PushXml 안의 chunk 결정에 boundary 보정
size_t chunk = std::min( remaining, payload_max );
if( chunk < remaining ) {  // 마지막 fragment 가 아니면
    // UTF-8 continuation byte (10xxxxxx) 면 뒤로 후퇴
    while( chunk > 0 && ( data[chunk] & 0xC0 ) == 0x80 ) {
        --chunk;
    }
}
```

추가 LOC: +8

### 13.7 🟡 Thread contract — 단일 producer 보장

§5.1 의 "Thread-safe" 표기는 misleading. 동시 PushXml 호출 시 fragment seq 순서 깨질 수 있음:

| t | thread A | thread B |
|---|---|---|
| 1 | PushPacket(frag1) seq=N | |
| 2 | | PushPacket(frag1) seq=N+1 |
| 3 | PushPacket(frag2 marker=1) seq=N+2 | |
| 4 | | PushPacket(frag2 marker=1) seq=N+3 |

→ 수신측이 marker bit 두 번 보고 XML 합쳐 파싱 시 실패.

**실제 사용 패턴 검증:**
- 4cam = 4 인스턴스. 각 cam 안에서 `SendDetectResultToMetaData` 는 **InferenceThread 1개**가 호출
- → 단일 producer 보장. race 없음.

**문서 명시:**
> **호출자 계약**: 한 `ONVIFMetadataPayloader` 인스턴스의 `PushXml()` 은 단일 producer 스레드에서만 호출. 다중 producer 필요 시 외부 mutex 또는 `SafeQueue` 로 직렬화.

### 13.8 🟢 검증 순서 재조정 (§9 수정)

원래 §9.3 은 VLC 우선, ODM 보조. **순서 바꾸기**:

| 순위 | 도구 | 이유 |
|---|---|---|
| 1 | **gst-launch + 자체 rtpdec** | 가장 확정적, 외부 의존 0 |
| 2 | **ONVIF Device Manager (Windows)** | 공식 ONVIF 도구, metadata 시각화 표시 |
| 3 | **Wireshark** | RTP/RTCP 정합성 (NTP-RTP, marker, seq) |
| 4 | VLC 4.x | 보조 — ONVIF metadata 지원 미흡할 수 있음 |

### 13.9 LOC 영향 정리

| 항목 | LOC |
|---|---|
| 13.2 constructor 초기화 명시 | +10 |
| 13.3 MetricsRegistry API 수정 (멤버 제거) | −15 |
| 13.4 appsrc 속성 (pipeline 셋업 측, payloader 외부) | 0 |
| 13.5 RTCP (GStreamer 자동) | 0 |
| 13.6 UTF-8 boundary 보강 | +8 (선택) |
| 13.7 thread contract 주석 | +5 |
| **최종** | **240 → 약 248** |

여전히 작다.

### 13.10 PoC Phase 1 일정 재산정

- **Day 0 (신규, 0.5일)** — gst-launch 통합 검증 (옵션 A 확인). 실패 시 결정 1 재방문.
- **Day 1 (1일)** — 페이로더 구현 (보강 사항 반영, ~248 LOC). 단위 테스트.
- **Day 2 (1일)** — ODM + Wireshark + VLC 호환성 검증. 메트릭 검증.

**총 2.5일** (당초 2일 → +0.5일).

### 13.11 결론

설계의 큰 줄기 (RTP 포맷, marker bit, fragmentation, 인터페이스 보존) 는 **모두 맞다**.

**Critical issue 1건 (13.1)** 만 PoC Phase 1 Day 0 검증으로 해소하면 안전 진입 가능.

나머지 6건은 구현 시 반영하면 됨.

---

Sources:
- [ONVIF Streaming Spec v25.12](https://www.onvif.org/specs/stream/ONVIF-Streaming-Spec.pdf)
- [ONVIF Streaming Spec v2.10](https://www.onvif.org/specs/stream/ONVIF-Streaming-Spec-v210.pdf)
- [RFC 3550 - RTP](https://datatracker.ietf.org/doc/html/rfc3550)
- [RFC 1952 - GZIP (we don't use)](https://datatracker.ietf.org/doc/html/rfc1952)
- [gst-rtsp-server rtsp-media-factory docs](https://gstreamer.freedesktop.org/documentation/gst-rtsp-server/rtsp-media-factory.html)
- [gst-rtsp-server test-appsrc.c (raw media 패턴)](https://github.com/GStreamer/gst-rtsp-server/blob/master/examples/test-appsrc.c)
- [GStreamer Discourse: appsrc 로 RTP 직접 push](https://discourse.gstreamer.org/t/how-to-pass-rtp-packets-to-appsrc/4655)
- [GStreamer rtpsession (RTCP 자동)](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpsession.html)
- [Axis ONVIF Replay Extension (NTP-RTP 매핑)](https://developer.axis.com/vapix/network-video/onvif-replay-extension/)
