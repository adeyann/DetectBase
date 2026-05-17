/**
 * @file OnvifMetadataPayloader.cpp
 * @brief ONVIF Metadata RTP 페이로더 구현.
 */

#include "OnvifMetadataPayloader.h"

#include "MgenLogger.h"
#include "MetricsRegistry.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace MGEN
{
    namespace
    {
        constexpr const char* METRIC_PACKETS_TOTAL = "detectbase_onvif_metadata_packets_total";
        constexpr const char* METRIC_BYTES_TOTAL   = "detectbase_onvif_metadata_bytes_total";
        constexpr const char* METRIC_DROPS_TOTAL   = "detectbase_onvif_metadata_drops_total";

        const std::map<std::string, std::string> NO_LABELS;

        bool g_metrics_registered = false;

        void RegisterMetricsOnce() noexcept
        {
            if( g_metrics_registered ) return;
            auto& reg = MetricsRegistry::Instance();
            reg.RegisterCounter( METRIC_PACKETS_TOTAL, "ONVIF metadata RTP packets sent" );
            reg.RegisterCounter( METRIC_BYTES_TOTAL,   "ONVIF metadata RTP bytes sent (incl. 12-byte RTP header)" );
            reg.RegisterCounter( METRIC_DROPS_TOTAL,   "ONVIF metadata push failures (appsrc null / GST_FLOW_* error)" );
            g_metrics_registered = true;
        }
    } // anonymous namespace

    OnvifMetadataPayloader::OnvifMetadataPayloader( GstAppSrc* appsrc ) noexcept
        : OnvifMetadataPayloader( appsrc, Config{} ) {}

    OnvifMetadataPayloader::OnvifMetadataPayloader( GstAppSrc* appsrc, const Config& cfg ) noexcept
        : appsrc_    ( appsrc )
        , cfg_       ( cfg )
        , ssrc_      ( 0 )
        , base_time_ ( std::chrono::steady_clock::now() )
    {
        std::random_device rd;
        std::mt19937       gen( rd() );

        ssrc_ = ( cfg.ssrc != 0 )
            ? cfg.ssrc
            : std::uniform_int_distribution<uint32_t>()( gen );

        seq_num_.store( std::uniform_int_distribution<uint16_t>()( gen ) );

        RegisterMetricsOnce();

        MLOG_INFO( "OnvifMetadataPayloader 생성 — appsrc=%p ssrc=0x%08x seq_start=%u mtu=%zu pt=%u",
            static_cast<void*>(appsrc_), ssrc_, seq_num_.load(), cfg_.mtu, cfg_.payload_type );
    }

    OnvifMetadataPayloader::~OnvifMetadataPayloader()
    {
        MLOG_INFO( "OnvifMetadataPayloader 종료 — packets=%lu bytes=%lu drops=%lu",
            static_cast<unsigned long>( packet_count_.load() ),
            static_cast<unsigned long>( byte_count_.load() ),
            static_cast<unsigned long>( drop_count_.load() ) );
    }

    uint32_t OnvifMetadataPayloader::Now90kHz() const noexcept
    {
        const auto    now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - base_time_ ).count();
        const int64_t ticks  = ( now_ns * 90 ) / 1000000;
        return static_cast<uint32_t>( ticks & 0xFFFFFFFF );
    }

    void OnvifMetadataPayloader::WriteRtpHeader( uint8_t* buf, bool marker, uint32_t timestamp ) noexcept
    {
        const uint16_t seq = seq_num_.fetch_add( 1 );

        buf[ 0 ] = 0x80;
        buf[ 1 ] = ( marker ? 0x80 : 0x00 ) | ( cfg_.payload_type & 0x7F );
        buf[ 2 ] = static_cast<uint8_t>( seq >> 8 );
        buf[ 3 ] = static_cast<uint8_t>( seq & 0xFF );
        buf[ 4 ] = static_cast<uint8_t>( timestamp >> 24 );
        buf[ 5 ] = static_cast<uint8_t>( timestamp >> 16 );
        buf[ 6 ] = static_cast<uint8_t>( timestamp >>  8 );
        buf[ 7 ] = static_cast<uint8_t>( timestamp & 0xFF );
        buf[  8 ] = static_cast<uint8_t>( ssrc_ >> 24 );
        buf[  9 ] = static_cast<uint8_t>( ssrc_ >> 16 );
        buf[ 10 ] = static_cast<uint8_t>( ssrc_ >>  8 );
        buf[ 11 ] = static_cast<uint8_t>( ssrc_ & 0xFF );
    }

    bool OnvifMetadataPayloader::PushPacket( const uint8_t* payload, size_t size, bool marker, uint32_t ts ) noexcept
    {
        if( !appsrc_ ) return false;

        const size_t total_size = 12 + size;
        GstBuffer*   buf        = gst_buffer_new_allocate( nullptr, total_size, nullptr );
        if( !buf ) return false;

        GstMapInfo info;
        if( !gst_buffer_map( buf, &info, GST_MAP_WRITE ) ) {
            gst_buffer_unref( buf );
            return false;
        }

        WriteRtpHeader( info.data, marker, ts );
        if( size > 0 && payload != nullptr ) {
            std::memcpy( info.data + 12, payload, size );
        }

        gst_buffer_unmap( buf, &info );

        GstFlowReturn fr = gst_app_src_push_buffer( appsrc_, buf );
        if( fr != GST_FLOW_OK ) {
            return false;
        }

        packet_count_.fetch_add( 1 );
        byte_count_.fetch_add( total_size );
        MetricsRegistry::Instance().IncrementCounter( METRIC_PACKETS_TOTAL, NO_LABELS, 1.0 );
        MetricsRegistry::Instance().IncrementCounter( METRIC_BYTES_TOTAL,   NO_LABELS, static_cast<double>( total_size ) );
        return true;
    }

    size_t OnvifMetadataPayloader::SafeUtf8Chunk( const uint8_t* data, size_t max_chunk, size_t remaining ) noexcept
    {
        if( max_chunk >= remaining ) return remaining;

        size_t chunk = max_chunk;
        // UTF-8 continuation byte 는 10xxxxxx (0x80~0xBF). multi-byte 문자 중간에서 자르지 않도록 후퇴.
        while( chunk > 0 && ( data[ chunk ] & 0xC0 ) == 0x80 ) {
            --chunk;
        }
        if( chunk == 0 ) chunk = max_chunk;
        return chunk;
    }

    bool OnvifMetadataPayloader::PushXml( const std::string& xml_document ) noexcept
    {
        if( !appsrc_ || xml_document.empty() ) {
            drop_count_.fetch_add( 1 );
            MetricsRegistry::Instance().IncrementCounter( METRIC_DROPS_TOTAL, NO_LABELS, 1.0 );
            return false;
        }

        const size_t payload_max = ( cfg_.mtu > 12 ) ? ( cfg_.mtu - 12 ) : 1;
        const uint32_t ts        = Now90kHz();

        const uint8_t* data      = reinterpret_cast<const uint8_t*>( xml_document.data() );
        size_t         remaining = xml_document.size();

        while( remaining > 0 ) {
            const size_t chunk  = SafeUtf8Chunk( data, payload_max, remaining );
            const bool   marker = ( chunk == remaining );

            if( !PushPacket( data, chunk, marker, ts ) ) {
                drop_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_DROPS_TOTAL, NO_LABELS, 1.0 );
                return false;
            }
            data      += chunk;
            remaining -= chunk;
        }
        return true;
    }

} // namespace MGEN
