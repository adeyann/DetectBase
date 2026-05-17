/**
 * @file OnvifMetadataPayloader.h
 * @brief ONVIF Metadata RTP 페이로더 — XML document 1개를 N개 RTP packet 으로 변환하여 GstAppSrc 에 push.
 *
 * @details
 * ONVIF Streaming Spec v25.12 준수:
 *   - encoding-name = "VND.ONVIF.METADATA"
 *   - clock-rate    = 90,000 Hz
 *   - 마지막 RTP packet 의 marker bit = 1 (XML document 끝 표시)
 *   - 크기 무제한 (MTU 초과 시 fragmentation, UTF-8 boundary 보호)
 *
 * 사용 패턴 (gst-rtsp-server media-configure 콜백 내부):
 *   1) factory 의 media-configure 시그널 핸들러에서 appsrc(name="pay1") 핸들 획득
 *   2) OnvifMetadataPayloader 생성자에 appsrc 전달
 *   3) NPU 감지 결과 발생 시 RtspDetectorUnit::SendDetectResultToMetaData → PushXml() 호출
 *
 * @note 호출자 계약: 한 인스턴스의 PushXml() 은 단일 producer 스레드에서만 호출.
 *       다중 producer 필요 시 외부 mutex 또는 SafeQueue 로 직렬화.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// GStreamer forward declarations — 헤더 의존성 누설 방지.
struct _GstAppSrc;
using GstAppSrc = struct _GstAppSrc;

namespace MGEN
{
    /**
     * @brief ONVIF Metadata RTP 페이로더 (단일 producer, thread-safe 한 push 보장 안 함).
     */
    class OnvifMetadataPayloader
    {
    public:
        /**
         * @brief 페이로더 구성 (모두 기본값 사용 가능).
         */
        struct Config
        {
            uint32_t ssrc          = 0;       ///< 0 = constructor 에서 random 생성 (RFC 3550 §3 권장)
            uint8_t  payload_type  = 97;      ///< dynamic RTP payload type 96~127 (관례: ONVIF=97)
            size_t   mtu           = 1400;    ///< RTP 페이로드 최대 크기 (이더넷 1500 - IP/UDP/RTP 헤더 여유)
        };

        /**
         * @param appsrc 외부 소유 (보통 GstRtspProxyServer). weak ref — payloader 가 생존 동안 appsrc 가 살아있어야 한다.
         * @param cfg    선택. 기본값 사용 가능.
         */
        explicit OnvifMetadataPayloader( GstAppSrc* appsrc, const Config& cfg ) noexcept;

        /** Default Config 로 생성 (편의 overload). */
        explicit OnvifMetadataPayloader( GstAppSrc* appsrc ) noexcept;

        ~OnvifMetadataPayloader();

        OnvifMetadataPayloader( const OnvifMetadataPayloader& )            = delete;
        OnvifMetadataPayloader& operator=( const OnvifMetadataPayloader& ) = delete;
        OnvifMetadataPayloader( OnvifMetadataPayloader&& )                 = delete;
        OnvifMetadataPayloader& operator=( OnvifMetadataPayloader&& )      = delete;

        /**
         * @brief XML document 한 개를 RTP 패킷 시퀀스로 변환하여 appsrc 에 push.
         *
         * - size ≤ (mtu - 12): 단일 packet, marker=1
         * - size >  (mtu - 12): N packet, 마지막만 marker=1
         * - UTF-8 multi-byte 문자 중간 절단 방지
         */
        bool PushXml( const std::string& xml_document ) noexcept;

        // 통계
        uint64_t GetPacketCount() const noexcept { return packet_count_.load(); }
        uint64_t GetByteCount()   const noexcept { return byte_count_.load();   }
        uint64_t GetDropCount()   const noexcept { return drop_count_.load();   }
        uint16_t GetCurrentSeq()  const noexcept { return seq_num_.load();      }
        uint32_t GetSsrc()        const noexcept { return ssrc_;                }

    private:
        static size_t SafeUtf8Chunk( const uint8_t* data, size_t max_chunk, size_t remaining ) noexcept;
        void   WriteRtpHeader( uint8_t* buf, bool marker, uint32_t timestamp ) noexcept;
        bool   PushPacket( const uint8_t* payload, size_t size, bool marker, uint32_t timestamp ) noexcept;
        uint32_t Now90kHz() const noexcept;

        GstAppSrc* appsrc_;                                 ///< weak ref. owner: GstRtspProxyServer factory
        Config     cfg_;
        uint32_t   ssrc_;
        std::chrono::steady_clock::time_point base_time_;

        std::atomic<uint16_t> seq_num_;
        std::atomic<uint64_t> packet_count_ { 0 };
        std::atomic<uint64_t> byte_count_   { 0 };
        std::atomic<uint64_t> drop_count_   { 0 };
    };

} // namespace MGEN
