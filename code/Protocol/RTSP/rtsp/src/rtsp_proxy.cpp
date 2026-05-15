#include "rtsp_proxy.h"

#include "rtsp_cfg.h"
#include "media_format.h"
#include "media_util.h"
#include "h264_util.h"
#include "h265_util.h"
#include "bit_vector.h"
#include "mjpeg.h"
#include "mpeg4.h"

#include <iostream>
#include <future>

// #include "SettingManager.h"
#include "MgenLogger.h"

#define AVFRAME_FREE( frame ) { if( frame != nullptr ){ av_frame_free( &(frame) ); } }

RTSP_PROXY* rtsp_add_proxy( RTSP_PROXY** p_proxy )
{
    RTSP_PROXY* p_tmp       = NULL;
    RTSP_PROXY* p_new_proxy = ( RTSP_PROXY* ) malloc( sizeof( RTSP_PROXY ) );

    // check memory allocate
    if( NULL == p_new_proxy )
        return NULL;

    // clear
    memset( p_new_proxy, 0, sizeof( RTSP_PROXY ) );

    // set at linked list
    p_tmp = *p_proxy;
    if( NULL == p_tmp ) {
        *p_proxy = p_new_proxy;
    }
    else {
        while( p_tmp && p_tmp->next ){
            p_tmp = p_tmp->next;
        }
        p_tmp->next = p_new_proxy;
    }
    return p_new_proxy;
}

void rtsp_free_proxies( RTSP_PROXY** p_proxy )
{
    RTSP_PROXY* p_next;
    RTSP_PROXY* p_tmp = *p_proxy;

    while( p_tmp ){
        p_next = p_tmp->next;

        if( p_tmp->proxy ){
            // ���� CRtspProxyDetect �� ��ĳ�����ؼ� CRtspProxy* �� �Ҵ��߱� ������
            // �Ҹ��� ȣ��� �����ϰ� �θ� Ŭ������ ȣ���ؾ� ��
            auto*  parent_proxy = static_cast<CRtspProxy*>(p_tmp->proxy);
            delete parent_proxy;
            p_tmp->proxy = NULL;
        }
        free( p_tmp );
        p_tmp = p_next;
    }
    *p_proxy = NULL;
}

const int rtsp_get_proxy_nums()
{
    RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
    int			nums    = 0;
    while( p_proxy ){
        p_proxy = p_proxy->next;
        nums++;
    }
    return nums;
}

RTSP_PROXY* rtsp_proxy_match( const char* suffix )
{
    RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
    while( p_proxy ){
        if( strcmp( suffix, p_proxy->cfg.suffix ) == 0 )
            return p_proxy;
        p_proxy = p_proxy->next;
    }
    return NULL;
}

CRtspProxy::CRtspProxy( PROXY_CFG* config ) noexcept
    : m_has_video                      ( 0 )
    , m_has_audio                      ( 0 )
    , m_inited                         ( 0 )
    , m_p_config                       ( config )
    , m_v_codec                        ( VIDEO_CODEC_NONE )
    , m_v_width                        ( 0 )
    , m_v_height                       ( 0 )
    , m_a_codec                        ( AUDIO_CODEC_NONE )
    , m_a_samplerate                   ( 0 )
    , m_a_channels                     ( 0 )
    , m_rtsp                           ( nullptr )
    , m_mjpeg                          ( nullptr )
    , m_notify_queue                   ( make_shared<SafeQueue<int>>() )
    , m_recv_video                     ( false )
    , m_is_terminate                   ( false )
    , m_last_avframe_queue_insert_time ( std::chrono::high_resolution_clock::now() )
    , m_packets_queue_                 ( make_shared<SafeQueue<AVPacket*>>() )
{
    memset( m_url,          0, sizeof( m_url )          );
    memset( m_user,         0, sizeof( m_user )         );
    memset( m_pass,         0, sizeof( m_pass )         );
    memset( m_redirect_url, 0, sizeof( m_redirect_url ) );

    m_p_callback_list = h_list_create( false );
    m_proxy_id        = atoi( config->suffix );
    m_recorder        = std::make_unique<CRtspProxyRecorder>( m_proxy_id );

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    m_n_video_recodec = PROXY_RECODEC_STILL_NOT_SURE;
    m_n_audio_recodec = PROXY_RECODEC_STILL_NOT_SURE;
    m_p_video_decoder = nullptr;
    m_p_audio_decoder = nullptr;
    m_p_video_encoder = nullptr;
    m_p_audio_encoder = nullptr;
#endif

    m_notify_thread_.SetThreadFunctions(
        std::bind( &CRtspProxy::NotifyThreadRunner, this ),
        std::bind( &CRtspProxy::NotifyThreadCloser, this )
    );

    m_decode_thread_.SetThreadFunctions(
        std::bind( &CRtspProxy::DecodeThreadRunner, this ),
        std::bind( &CRtspProxy::DecodeThreadCloser, this )
    );
}

CRtspProxy::~CRtspProxy()
{
    auto future = std::async( std::launch::async, [this]() { this->freeConn(); } );
    if ( future.wait_for( std::chrono::seconds(3) ) == std::future_status::timeout ){
        std::cerr << ">> Proxy [" << m_proxy_id << "] Delete TIEMOUT [ 3s ]" << std::endl;
    }
    this->m_notify_thread_.Stop();
    this->m_decode_thread_.Stop();

    if( this->m_packets_queue_ )
    {
        this->m_packets_queue_->clear_with_action(
            []( AVPacket* pkt ){
                if( pkt ){
                    av_packet_free( &pkt );
                }
            }
        );
    }

    int proxy_toss_status = m_init_regist_decoded_frame_queue.load();
    if( proxy_toss_status == ProxyTossInitialized || proxy_toss_status == ProxyTossRunnable ) {
        m_decoded_avframes = nullptr;
        m_init_regist_decoded_frame_queue.store( ProxyTossDeleted );
    }

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if( m_p_video_decoder ){
        delete m_p_video_decoder;
        m_p_video_decoder = NULL;
    }
    if( m_p_audio_decoder ){
        delete m_p_audio_decoder;
        m_p_audio_decoder = NULL;
    }
    if( m_p_video_encoder ){
        delete m_p_video_encoder;
        m_p_video_encoder = NULL;
    }
    if( m_p_audio_encoder ){
        delete m_p_audio_encoder;
        m_p_audio_encoder = NULL;
    }
#endif
    h_list_free_container( m_p_callback_list );
}

void CRtspProxy::onNotify( int evt )
{
    if( m_is_terminate == true )
        return;

    switch( evt )
    {
    case RTSP_EVE_CONNSUCC:
        m_v_codec = m_rtsp->video_codec();
        m_a_codec = m_rtsp->audio_codec();

        if( m_v_codec != VIDEO_CODEC_NONE )
            m_has_video = 1;

        if( m_a_codec != AUDIO_CODEC_NONE ) {
            m_has_audio    = 1;
            m_a_samplerate = m_rtsp->get_audio_samplerate();
            m_a_channels   = m_rtsp->get_audio_channels();
        }
        m_inited = 1;
        break;

    case MJPEG_EVE_CONNSUCC:
        m_v_codec   = VIDEO_CODEC_JPEG;
        m_has_video = 1;
        m_inited    = 1;
        break;

    default:
        break;
    }
    m_notify_queue->enqueue_copy( evt );
}

void CRtspProxy::onAudio( uint8* pdata, int len, uint32 ts, uint16 seq )
{
    if( m_is_terminate == true )
        return;

    UNUSED( ts );

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    const int need_recodec = needAudioRecodec();
    switch( need_recodec )
    {
    case PROXY_RECODEC_NEED:
        audioDataDecode( pdata, len );
        break;

    case PROXY_RECODEC_NOT_NEED:
        runCallBacks( pdata, len, DATA_TYPE_AUDIO );
        break;

    case PROXY_RECODEC_STILL_NOT_SURE:
        return;
    }
#else
    runCallBacks( pdata, len, DATA_TYPE_AUDIO );
#endif
}

void CRtspProxy::onVideo( uint8* pdata, int len, uint32 ts, uint16 seq )
{
    if( m_is_terminate == true )
        return;

    // m_v_width / height 에 접근하는 모든 로직을 락으로 보호
    unique_lock<mutex> lck( m_recv_video_mtx );
    m_recv_video = true;

    // video size check
    if( m_v_width == 0 || m_v_height == 0 ) {
        // Try parse video size
        if( parseVideoSize( pdata, len ) == -1 ) {
            lck.unlock();
            return;
        }
    }

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    // NORMAL CASE => detect : NEED_DETECT & recodec : PROXY_RECODEC_NOT_NEED
    int need_recodec = needVideoRecodec( pdata, len );
    UNUSED( need_recodec );
#endif

    ProxyAVFrameToss need_toss = needAVFrameToss( pdata, len );

    // 무거운 콜백/디코딩 함수 호출 전에 락을 해제
    lck.unlock();

    switch( need_toss )
    {
    default:
    case ProxyAVFrameToss::NotSure:
        break;

    case ProxyAVFrameToss::NotNeed:
        break;

    case ProxyAVFrameToss::Need:
        if( m_decoded_avframes != nullptr || containsSPSorPPS( pdata, len ) ) {
            videoDataDecode( pdata, len );
        }
        break;
    }
    runCallBacks( pdata, len, DATA_TYPE_VIDEO );
}

static std::string GetRtspEventNotifyName( const int evt )
{
    switch( evt )
    {
    case RTSP_EVE_STOPPED:    return std::string {"RTSP_EVE_STOPPED"};
    case RTSP_EVE_CONNECTING: return std::string {"RTSP_EVE_CONNECTING"};
    case RTSP_EVE_CONNFAIL:   return std::string {"RTSP_EVE_CONNFAIL"};
    case RTSP_EVE_CONNSUCC:   return std::string {"RTSP_EVE_CONNSUCC"};
    case RTSP_EVE_NOSIGNAL:   return std::string {"RTSP_EVE_NOSIGNAL"};
    case RTSP_EVE_RESUME:     return std::string {"RTSP_EVE_RESUME"};
    case RTSP_EVE_AUTHFAILED: return std::string {"RTSP_EVE_AUTHFAILED"};
    case RTSP_EVE_NODATA:     return std::string {"RTSP_EVE_NODATA"};
    default:                  return std::string {"NONE_SET_EVE"};
    }
}

void CRtspProxy::NotifyThreadRunner( void )
{
    auto&  running = m_notify_thread_.GetRunningFlag();
    while( running.load() == true ) {
        // dequeue_wait_for: 종료/타임아웃 시 std::nullopt 반환 (throw 없음).
        auto opt = m_notify_queue->dequeue_wait_for( std::chrono::milliseconds( 100 ) );
        if( !opt.has_value() ) {
            continue;
        }
        const int evt = *opt;

        if( evt < 0 ) break;
        if( evt == RTSP_EVE_CONNFAIL  || evt == RTSP_EVE_NOSIGNAL  ||
            evt == RTSP_EVE_NODATA    || evt == RTSP_EVE_STOPPED   ||
            evt == MJPEG_EVE_CONNFAIL || evt == MJPEG_EVE_NOSIGNAL ||
            evt == MJPEG_EVE_NODATA   || evt == MJPEG_EVE_STOPPED  ){

            unique_lock<mutex> lck( m_recv_video_mtx );
            m_recv_video = false;
            lck.unlock();

            // MLOG_INFO("RTSP Stream <%s> event occured : %s", m_p_config->suffix, GetRtspEventNotifyName(evt).c_str());
            this->freeConn(); // restartConn() 함수를 분할해서 시전

            std::unique_lock<std::mutex> cond_lck { this->m_notify_thread_mtx_ };
            this->m_notify_thread_cond_.wait_for( cond_lck, std::chrono::seconds( 1 ), [&] {
                return running.load() == false; }
            );

            if( running.load() == false )
                break;

            this->clearNotify();
            this->reconnectAfterFreeConnect();
        }
    }

    m_notify_queue->clear_with_action(
        []( int evt_dq ) {
            UNUSE_PARAM(evt_dq);
        }
    );
}

void CRtspProxy::NotifyThreadCloser( void )
{
    std::unique_lock<std::mutex> lck { this->m_notify_thread_mtx_ };
    this->m_notify_thread_cond_.notify_all();
    lck.unlock();
    m_notify_queue->terminate();
}

static void av_packet_deleter( AVPacket* pkt )
{
    if( pkt ) {
        av_packet_free( &pkt );
    }
}

void CRtspProxy::DecodeThreadRunner( void )
{
    std::optional<AVPacket*> pkt_opt = std::nullopt;

    auto&  running = m_decode_thread_.GetRunningFlag();
    while( running.load() == true )
    {
        pkt_opt = std::nullopt;
        try
        {
            pkt_opt = m_packets_queue_->dequeue_wait_for( 10s );
        }
        catch( ... )
        {
            std::this_thread::sleep_for( 10ms );
            continue;
        }

        if( pkt_opt.has_value() == false ){
            // case 1. terminate
            if( m_packets_queue_->is_terminated() ){
                continue;
            }
            // case 2. timeout
            continue;
        }

        AVPacket* pkt = *pkt_opt;

        if( pkt == nullptr ){
            continue;
        }

        // RAII를 위해 unique_ptr로 pkt의 소유권을 관리합니다.
        // 이 스코프를 벗어나면(continue, return, exception) 자동으로 av_packet_free가 호출됩니다.
        std::unique_ptr<AVPacket, decltype(&av_packet_deleter)> pkt_guard( pkt, &av_packet_deleter );

        try
        {
            if( m_p_video_decoder ) {
                m_p_video_decoder->decode( pkt_guard.get() );
            }
        }
        catch (const std::exception& e) {
            MLOG_ERROR("CAM<%s> decode exception: %s", m_p_config->suffix, e.what());
        }
        catch (...) {
            MLOG_ERROR("CAM<%s> decode unknown exception", m_p_config->suffix);
        }
    }
    // MLOG_INFO("Decoder thread runner stopped for %s", m_p_config->suffix);
}

void CRtspProxy::DecodeThreadCloser( void )
{
    this->m_packets_queue_->terminate();
}

static int event_notify_cb( int evt, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->onNotify( evt );
    return 0;
}

static int rtsp_video_cb( uint8* p_data, int len, uint32 ts, uint16 seq, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->onVideo( p_data, len, ts, seq );
    return 0;
}

static int rtsp_audio_cb( uint8* p_data, int len, uint32 ts, uint16 seq, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->onAudio( p_data, len, ts, seq );
    return 0;
}

static int rtsp_redirect_cb( char* url, void* p_user )
{
    auto* p_proxy = static_cast< CRtspProxy* >( p_user );

    // Clear & Set
    memset ( p_proxy->m_redirect_url, 0x00, sizeof( p_proxy->m_redirect_url ) );
    strncpy( p_proxy->m_redirect_url, url,  sizeof( p_proxy->m_redirect_url ) - 1 );

    MLOG_INFO( "RTSP Redirect new URL : %s", p_proxy->m_redirect_url );
    return 0;
}

static int jpeg_video_cb( uint8* p_data, int len, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->onVideo( p_data, len, 0, 0 );
    return 0;
}

void rtsp_proxy_video_decoder_cb( AVFrame* frame, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->avframeTossCallback( frame, DATA_TYPE_IMAGE );
}

static void rtsp_proxy_video_encoder_cb( uint8* data, int size, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->runCallBacks( data, size, DATA_TYPE_VIDEO );
}

static void rtsp_proxy_audio_decoder_cb( AVFrame* frame, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->audioDataEncode( frame ); // Encode.....???? ���Ž� �ڵ忡 �׷��� ��������
}

static void rtsp_proxy_audio_encoder_cb( uint8* data, int size, int nbsamples, void* p_user )
{
    static_cast< CRtspProxy* >( p_user )->runCallBacks( data, size, DATA_TYPE_AUDIO ); // �굵 ����Ʈ Ÿ�� �� �Ƴ�?
}

static const bool check_url_is_rtsp_type( const char* url )
{
    return ( memcmp( url, "rtsp://", 7 ) == 0 );
}

static const bool check_url_is_http_s_type( const char* url )
{
    return ( memcmp( url, "http://", 7 ) == 0 || memcmp( url, "https://", 8 ) == 0 );
}

const bool CRtspProxy::startConn( const char* url, char* user, char* pass )
{
    this->m_notify_thread_.Start();
    this->m_decode_thread_.Start();

    if( check_url_is_rtsp_type( url ) == true ){
        m_rtsp = new CRtsp;
        if( NULL == m_rtsp ){
            MLOG_ERROR( "%s() => new rtsp failed (%s)", __func__, url );
            return false;
        }
        // Set CRtsp Callback
        m_rtsp->set_notify_cb  ( event_notify_cb, this );
        m_rtsp->set_video_cb   ( rtsp_video_cb    );
        m_rtsp->set_audio_cb   ( rtsp_audio_cb    );
        m_rtsp->set_redirect_cb( rtsp_redirect_cb );

        // Set value after clear
        memset( m_url,  0x00, sizeof( m_url  ) );
        memset( m_user, 0x00, sizeof( m_user ) );
        memset( m_pass, 0x00, sizeof( m_pass ) );

        strncpy( m_url,  url,  sizeof( m_url  ) - 1 );
        strncpy( m_user, user, sizeof( m_user ) - 1 );
        strncpy( m_pass, pass, sizeof( m_pass ) - 1 );

        const bool rtsp_status = m_rtsp->rtsp_start( m_url, m_user, m_pass );
        if( rtsp_status == false )
            MLOG_ERROR( "Camera [%s] Start Failed", m_p_config->suffix );
        return rtsp_status;
    }
    else if( check_url_is_http_s_type( url ) == true ) {
        m_mjpeg = new CHttpMjpeg;
        if( NULL == m_mjpeg ){
            MLOG_ERROR( "%s() => new http-mjpeg failed (%s)", __func__, url );
            return false;
        }
        m_mjpeg->set_notify_cb( event_notify_cb, this );
        m_mjpeg->set_video_cb ( jpeg_video_cb );

        // Set value after clear
        memset( m_url,  0x00, sizeof( m_url  ) );
        memset( m_user, 0x00, sizeof( m_user ) );
        memset( m_pass, 0x00, sizeof( m_pass ) );

        strncpy( m_url,  url,  sizeof( m_url  ) - 1 );
        strncpy( m_user, user, sizeof( m_user ) - 1 );
        strncpy( m_pass, pass, sizeof( m_pass ) - 1 );

        const bool mjpeg_status = m_mjpeg->mjpeg_start( m_url, m_user, m_pass );
        if( mjpeg_status == false )
            MLOG_ERROR( "URL:%s Mjpeg Start Failed", m_url );
        return mjpeg_status;
    }
    else {
        MLOG_TRACE( "RTSP URL is neither RTSP or HTTP" );
        MLOG_TRACE( "VMS NAME : %s", m_p_config->vmsName );

        m_rtsp = new CRtsp;
        if( NULL == m_rtsp ){
            MLOG_ERROR( "%s() => new rtsp failed (%s)", __func__, url );
            return false;
        }

        m_rtsp->set_notify_cb  ( event_notify_cb, this );
        m_rtsp->set_video_cb   ( rtsp_video_cb    );
        m_rtsp->set_audio_cb   ( rtsp_audio_cb    );
        m_rtsp->set_redirect_cb( rtsp_redirect_cb );
    }
    return false;
}

char* CRtspProxy::getH264AuxSDPLine( const int rtp_pt )
{
    char sdp[1024] = { '\0' };

    if( NULL == m_rtsp )
        return NULL;

    if( m_rtsp->get_h264_sdp_desc( sdp, sizeof( sdp ) ) == FALSE ){
        MLOG_ERROR( "%s() => CAM[%s] Get H264 SDP DESC Failed", __func__, m_p_config->suffix );
        return NULL;
    }

    char const* fmtpFmt = "a=fmtp:%d %s";
    uint32 fmtpFmtSize  = strlen( fmtpFmt )
                        + 3  /* max char len */
                        + strlen( sdp )
                        + 1; /* for \0 */

    char* fmtp = new char[fmtpFmtSize];
    memset( fmtp, 0, fmtpFmtSize );
    sprintf( fmtp, fmtpFmt, rtp_pt, sdp );

    return fmtp;
}

char* CRtspProxy::getH265AuxSDPLine( const int rtp_pt )
{
    char sdp[1024] = { '\0' };

    if( NULL == m_rtsp )
        return NULL;

    if( m_rtsp->get_h265_sdp_desc( sdp, sizeof( sdp ) ) == FALSE ) {
        MLOG_ERROR( "%s() => CAM[%s] Get H265 SDP DESC Failed", __func__, m_p_config->suffix );
        return NULL;
    }

    char const* fmtpFmt = "a=fmtp:%d %s";
    uint32 fmtpFmtSize  = strlen( fmtpFmt )
                        + 3  /* max char len */
                        + strlen( sdp )
                        + 1; /* for \0 */

    char* fmtp = new char[fmtpFmtSize];
    memset( fmtp, 0, fmtpFmtSize );
    sprintf( fmtp, fmtpFmt, rtp_pt, sdp );

    return fmtp;
}

char* CRtspProxy::getMP4AuxSDPLine( const int rtp_pt )
{
    char sdp[1024] = { '\0' };

    if( NULL == m_rtsp )
        return NULL;

    if( m_rtsp->get_mp4_sdp_desc( sdp, sizeof( sdp ) ) == FALSE ){
        MLOG_ERROR( "%s() => CAM[%s] Get MP4 SDP DESC Failed", __func__, m_p_config->suffix );
        return NULL;
    }

    char const* fmtpFmt = "a=fmtp:%d %s";
    uint32 fmtpFmtSize  = strlen( fmtpFmt )
                        + 3  /* max char len */
                        + strlen( sdp )
                        + 1; /* for \0 */

    char* fmtp = new char[fmtpFmtSize];
    memset( fmtp, 0, fmtpFmtSize );
    sprintf( fmtp, fmtpFmt, rtp_pt, sdp );

    return fmtp;
}

char* CRtspProxy::getAACAuxSDPLine( const int rtp_pt )
{
    char sdp[1024] = { '\0' };

    if( NULL == m_rtsp )
        return NULL;

    if( m_rtsp->get_aac_sdp_desc( sdp, sizeof( sdp ) ) == FALSE ){
        MLOG_ERROR( "%s() => CAM[%s] Get AAC SDP DESC Failed", __func__, m_p_config->suffix );
        return NULL;
    }

    char const* fmtpFmt = "a=fmtp:%d %s";
    uint32 fmtpFmtSize  = strlen( fmtpFmt )
                        + 3  /* max char len */
                        + strlen( sdp )
                        + 1; /* for \0 */

    char* fmtp = new char[fmtpFmtSize];
    memset( fmtp, 0, fmtpFmtSize );
    sprintf( fmtp, fmtpFmt, rtp_pt, sdp );

    return fmtp;
}

char* CRtspProxy::getVideoAuxSDPLine( int rtp_pt )
{
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if( m_p_video_encoder )
        return m_p_video_encoder->getAuxSDPLine( rtp_pt );
#endif
    switch( m_v_codec )
    {
    case VIDEO_CODEC_H264: return this->getH264AuxSDPLine( rtp_pt );
    case VIDEO_CODEC_H265: return this->getH265AuxSDPLine( rtp_pt );
    case VIDEO_CODEC_MP4:  return this->getMP4AuxSDPLine( rtp_pt );
    }
    return NULL;
}

char* CRtspProxy::getAudioAuxSDPLine( const int rtp_pt )
{
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if( m_p_audio_encoder )
        return m_p_audio_encoder->getAuxSDPLine( rtp_pt );
#endif
    if( m_a_codec == AUDIO_CODEC_AAC )
        return this->getAACAuxSDPLine( rtp_pt );
    return NULL;
}

const bool CRtspProxy::isCallbackExist( ProxyDataCB p_callback, void* p_userdata )
{
    bool         exist  = false;
    ProxyCB*     p_cb   = NULL;
    LINKED_NODE* p_node = NULL;

    lock_guard<mutex> lck( m_p_callback_mtx );
    p_node = h_list_lookup_start( m_p_callback_list );
    while( p_node ) {
        p_cb = static_cast< ProxyCB* >( p_node->p_data );
        if( p_cb->pCallback == p_callback && p_cb->pUserdata == p_userdata ) {
            exist = true;
            break;
        }
        p_node = h_list_lookup_next( m_p_callback_list, p_node );
    }
    h_list_lookup_end( m_p_callback_list );
    return exist;
}

void CRtspProxy::addCallback( ProxyDataCB pCallback, void* pUserdata )
{
    if( isCallbackExist( pCallback, pUserdata ) ){
        MLOG_TRACE( "CAM[%s] : rtsp_proxy callback already exists so was not added", m_p_config->suffix );
        return;
    }

    ProxyCB* p_cb   = ( ProxyCB* ) malloc( sizeof( ProxyCB ) );
    p_cb->pCallback = pCallback;
    p_cb->pUserdata = pUserdata;
    p_cb->bFirst    = TRUE;

    lock_guard<mutex> lck( m_p_callback_mtx );
    h_list_add_at_back( m_p_callback_list, p_cb );
}

void CRtspProxy::delCallback( ProxyDataCB p_callback, void* p_userdata )
{
    ProxyCB*     p_cb   = NULL;
    LINKED_NODE* p_node = NULL;

    lock_guard<mutex> lck( m_p_callback_mtx );
    p_node = h_list_lookup_start( m_p_callback_list );
    while( p_node ){
        p_cb = static_cast< ProxyCB* >( p_node->p_data );
        if( p_cb->pCallback == p_callback && p_cb->pUserdata == p_userdata ) {
            free( p_cb );
            h_list_remove( m_p_callback_list, p_node );
            break;
        }
        p_node = h_list_lookup_next( m_p_callback_list, p_node );
    }
    h_list_lookup_end( m_p_callback_list );
}

void CRtspProxy::runCallBacks( uint8* data, int size, int type )
{
    ProxyCB*     p_cb   = NULL;
    LINKED_NODE* p_node = NULL;

    lock_guard<mutex> lck( m_p_callback_mtx );
    p_node = h_list_lookup_start( m_p_callback_list );
    while( p_node ){
        p_cb = static_cast< ProxyCB* >( p_node->p_data );
        if( p_cb->pCallback != NULL ) {
            if( p_cb->bFirst && type == DATA_TYPE_VIDEO ) {
                p_cb->bFirst = FALSE;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
                if( NULL == m_p_video_encoder )
#endif
                {
                    if( m_v_codec == VIDEO_CODEC_H264 ) {
                        uint8 sps[1024] = { 0x00, };
                        uint8 pps[1024] = { 0x00, };
                        int sps_len = 0, pps_len = 0;

                        // ���� get_h264_params() ������� true ��� �����ϴ� �ݹ��
                        if( m_rtsp->get_h264_params( sps, &sps_len, pps, &pps_len ) ){
                            p_cb->pCallback( sps, sps_len, type, p_cb->pUserdata );
                            p_cb->pCallback( pps, pps_len, type, p_cb->pUserdata );
                        }
                    }
                    else if( m_v_codec == VIDEO_CODEC_H265 ){
                        uint8 vps[1024] = { 0x00, };
                        uint8 sps[1024] = { 0x00, };
                        uint8 pps[1024] = { 0x00, };
                        int vps_len, sps_len = 0, pps_len = 0;

                        // ���� get_h265_params() ������� true ��� �����ϴ� �ݹ��
                        if( m_rtsp->get_h265_params( sps, &sps_len, pps, &pps_len, vps, &vps_len ) ) {
                            p_cb->pCallback( vps, vps_len, type, p_cb->pUserdata );
                            p_cb->pCallback( sps, sps_len, type, p_cb->pUserdata );
                            p_cb->pCallback( pps, pps_len, type, p_cb->pUserdata );
                        }
                    }
                }
            } // p_cb->bFirst && type == DATA_TYPE_VIDEO

            // type ����̴��� �ش� �ݹ��� �׻� �����.. �̰� �³�?
            p_cb->pCallback( data, size, type, p_cb->pUserdata );

        }
        p_node = h_list_lookup_next( m_p_callback_list, p_node );
    }
    h_list_lookup_end( m_p_callback_list );
}

ProxyAVFrameToss CRtspProxy::needAVFrameToss( uint8* p_data, int len )
{
    if( ProxyAVFrameToss::NotSure != m_n_avframe_toss ) {
        return m_n_avframe_toss;
    }

    if( m_v_width == 0 || m_v_height == 0 ) {
        return ProxyAVFrameToss::NotSure;
    }

    if( m_v_codec != VIDEO_CODEC_NONE )
    {
        if( m_p_video_decoder == nullptr )
        {
                // 1. 디코더 init에 필요한 파라미터 준비
                uint8_t   extradata_buf[2048] = { 0x00, }; // SPS/PPS를 결합할 임시 버퍼
                int       extradata_size = 0;
                AVCodecID codecId = AV_CODEC_ID_NONE;

                // FFmpeg extradata는 Annex B 형식 (00 00 00 01)을 따릅니다.
                const uint8_t start_code[] = { 0x00, 0x00, 0x00, 0x01 };
                const int start_code_size = sizeof(start_code);

                if (m_v_codec == VIDEO_CODEC_H264) {
                    codecId = AV_CODEC_ID_H264;
                    uint8 sps[1024] = {0}; int sps_len = 0;
                    uint8 pps[1024] = {0}; int pps_len = 0;

                    if (m_rtsp && m_rtsp->get_h264_params(sps, &sps_len, pps, &pps_len)) {
                        // extradata 버퍼에 [start_code][SPS][start_code][PPS] 순서로 조립
                        memcpy(extradata_buf + extradata_size, start_code, start_code_size);
                        extradata_size += start_code_size;
                        memcpy(extradata_buf + extradata_size, sps, sps_len);
                        extradata_size += sps_len;

                        memcpy(extradata_buf + extradata_size, start_code, start_code_size);
                        extradata_size += start_code_size;
                        memcpy(extradata_buf + extradata_size, pps, pps_len);
                        extradata_size += pps_len;
                    } else {
                        MLOG_ERROR( "%s() => CAM[%s], get_h264_params failed!", __func__, m_p_config->suffix );
                        m_n_avframe_toss = ProxyAVFrameToss::NotSure;
                        return m_n_avframe_toss;
                    }
                }
                else if (m_v_codec == VIDEO_CODEC_H265) {
                    codecId = AV_CODEC_ID_H265;
                    uint8 vps[1024] = {0}; int vps_len = 0;
                    uint8 sps[1024] = {0}; int sps_len = 0;
                    uint8 pps[1024] = {0}; int pps_len = 0;

                    if (m_rtsp && m_rtsp->get_h265_params(sps, &sps_len, pps, &pps_len, vps, &vps_len)) {
                        // extradata 버퍼에 [start_code][VPS][start_code][SPS][start_code][PPS] 순서로 조립
                        memcpy(extradata_buf + extradata_size, start_code, start_code_size);
                        extradata_size += start_code_size;
                        memcpy(extradata_buf + extradata_size, vps, vps_len);
                        extradata_size += vps_len;

                        memcpy(extradata_buf + extradata_size, start_code, start_code_size);
                        extradata_size += start_code_size;
                        memcpy(extradata_buf + extradata_size, sps, sps_len);
                        extradata_size += sps_len;

                        memcpy(extradata_buf + extradata_size, start_code, start_code_size);
                        extradata_size += start_code_size;
                        memcpy(extradata_buf + extradata_size, pps, pps_len);
                        extradata_size += pps_len;
                    } else {
                        MLOG_ERROR( "%s() => CAM[%s], get_h265_params failed!", __func__, m_p_config->suffix );
                        m_n_avframe_toss = ProxyAVFrameToss::NotSure;
                        return m_n_avframe_toss;
                    }
                }
                else if (m_v_codec == VIDEO_CODEC_JPEG) {
                    codecId = AV_CODEC_ID_MJPEG;
                    // MJPEG는 extradata가 필요 없음
                }
                else {
                    MLOG_ERROR( "%s() => CAM[%s], Unsupported codec type: %d", __func__, m_p_config->suffix, m_v_codec );
                    m_n_avframe_toss = ProxyAVFrameToss::NotSure;
                    return m_n_avframe_toss;
                }

            m_p_video_decoder = new CVideoDecoder;

            if( m_p_video_decoder == nullptr ) {
                MLOG_ERROR( "%s() => CAM[%s], new video decoder failed", __func__, m_p_config->suffix );
                m_n_avframe_toss = ProxyAVFrameToss::NotNeed;
                return m_n_avframe_toss;
            }

            CVideoDecoder::InitConfig init_config {
                .codec_id     = codecId,
                .v_width      = m_v_width,
                .v_height     = m_v_height,
                .p_extra_data = extradata_buf,
                .n_extra_size = extradata_size,
                .gpu_id       = DECODE_GPU_NOT_USE,
                .cam_id       = atoi(m_p_config->suffix)
            };

            //if( !m_p_video_decoder->init( m_v_codec, DECODE_GPU_NOT_USE ) ) {
            if( !m_p_video_decoder->init( init_config ) ){
                MLOG_ERROR( "%s() => CAM[%s], video decoder init failed", __func__, m_p_config->suffix );
                delete m_p_video_decoder;
                m_p_video_decoder = nullptr;
                m_n_avframe_toss = ProxyAVFrameToss::NotNeed;
                return m_n_avframe_toss;
            }

            m_p_video_decoder->setCallback( rtsp_proxy_video_decoder_cb, this );

            // Synchronize the constructed extradata with the recorder
            if( m_recorder && extradata_size > 0 ) {
                AVCodecParameters* params = avcodec_parameters_alloc();
                params->codec_type = AVMEDIA_TYPE_VIDEO;
                params->codec_id   = codecId;
                params->width      = m_v_width;
                params->height     = m_v_height;
                params->extradata  = ( uint8_t* ) av_mallocz( extradata_size + AV_INPUT_BUFFER_PADDING_SIZE );
                memcpy( params->extradata, extradata_buf, extradata_size );
                params->extradata_size = extradata_size;

                m_recorder->setVideoCodecParameters( params );
                avcodec_parameters_free( &params );
            }
        }

        // 차후 encoder 필요시 여기에?

        m_n_avframe_toss = ProxyAVFrameToss::Need;
    } else {
        m_n_avframe_toss = ProxyAVFrameToss::NotNeed;
    }
    return m_n_avframe_toss;
}

const int CRtspProxy::needVideoRecodec( uint8* p_data, int len )
{
    if( PROXY_RECODEC_STILL_NOT_SURE != m_n_video_recodec )
        return m_n_video_recodec;

    if( !m_p_config->has_output ) {
        m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
        return PROXY_RECODEC_NOT_NEED;
    }

    if( m_v_width == 0 || m_v_height == 0 )
        return 0;

    if( VIDEO_CODEC_NONE == m_p_config->output.v_info.codec )
        m_p_config->output.v_info.codec = m_v_codec;

    if( 0 == m_p_config->output.v_info.width )
        m_p_config->output.v_info.width = m_v_width;

    if( 0 == m_p_config->output.v_info.height )
        m_p_config->output.v_info.height = m_v_height;

    if( 0 == m_p_config->output.v_info.framerate )
        m_p_config->output.v_info.framerate = 30;

#if true
    m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
#else
    if( m_p_config->output.v_info.codec  != m_v_codec  ||
        m_p_config->output.v_info.width  != m_v_width  ||
        m_p_config->output.v_info.height != m_v_height ){

        if( !m_p_video_decoder ) {
            m_p_video_decoder = new CVideoDecoder;
            if( NULL == m_p_video_decoder ) {
                MLOG_ERROR( "%s() => CAM[%s], new video decoder failed", __func__, m_p_config->suffix );
                m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }

            if( !m_p_video_decoder->init( m_v_codec, DECODE_GPU_NOT_USE ) ) {
                MLOG_ERROR( "%s() => CAM[%s], video decoder init failed", __func__, m_p_config->suffix );
                m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }
            m_p_video_decoder->setCallback( rtsp_proxy_video_decoder_cb, this );
            MLOG_TRACE( "RTSP Proxy ( suffix:%s ) init video decoder", m_p_config->suffix );
        }

        if( !m_p_video_encoder ) {
            m_p_video_encoder = new CVideoEncoder;
            if( NULL == m_p_video_encoder ) {
                MLOG_ERROR( "%s() => CAM[%s], new video encoder failed", __func__, m_p_config->suffix );
                m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }

            VideoEncoderParam params;
            memset( &params, 0, sizeof( params ) );

            params.SrcWidth     = m_v_width;
            params.SrcHeight    = m_v_height;
            params.SrcPixFmt    = AV_PIX_FMT_YUV420P; // �̰� �ٲ� �� ������... ���ɿ� ����Ǵ� �� �ֳ�? YUV �ʿ��...��?
            params.DstCodec     = m_p_config->output.v_info.codec;
            params.DstWidth     = m_p_config->output.v_info.width;
            params.DstHeight    = m_p_config->output.v_info.height;
            params.DstFramerate = m_p_config->output.v_info.framerate;
            params.DstBitrate   = m_p_config->output.v_info.bitrate;

            if( FALSE == m_p_video_encoder->init( &params ) ) {
                MLOG_ERROR( "%s() => CAM[%s], video encoder init failed", __func__, m_p_config->suffix );
                m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }
            m_p_video_encoder->addCallback( rtsp_proxy_video_encoder_cb, this );
            MLOG_TRACE( "RTSP Proxy ( suffix:%s ) init video encoder", m_p_config->suffix );
        }
        m_n_video_recodec = PROXY_RECODEC_NEED;
    } // codec, width, height exit
    else
        m_n_video_recodec = PROXY_RECODEC_NOT_NEED;
#endif
    return m_n_video_recodec;
}

const int CRtspProxy::needAudioRecodec()
{
    if( PROXY_RECODEC_STILL_NOT_SURE != m_n_audio_recodec )
        return m_n_audio_recodec;

    if( !m_p_config->has_output ) {
        m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;
        return PROXY_RECODEC_NOT_NEED;
    }

    if( AUDIO_CODEC_NONE == m_p_config->output.a_info.codec )
        m_p_config->output.a_info.codec = m_a_codec;

    if( 0 == m_p_config->output.a_info.samplerate )
        m_p_config->output.a_info.samplerate = m_a_samplerate;

    if( 0 == m_p_config->output.a_info.channels )
        m_p_config->output.a_info.channels = m_a_channels;

    if( m_p_config->output.a_info.codec      != m_a_codec      ||
        m_p_config->output.a_info.samplerate != m_a_samplerate ||
        m_p_config->output.a_info.channels   != m_a_channels   ){

        if( !m_p_audio_decoder ) {
            m_p_audio_decoder = new CAudioDecoder;
            if( NULL == m_p_audio_decoder ) {
                MLOG_ERROR( "%s() => CAM[%s], new audio decoder failed", __func__, m_p_config->suffix );
                m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }

            if( !m_p_audio_decoder->init( m_a_codec, m_a_samplerate, m_a_channels, NULL, 0 ) ){
                MLOG_ERROR( "%s() => CAM[%s], audio decoder init failed", __func__, m_p_config->suffix );
                m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }
            m_p_audio_decoder->setCallback( rtsp_proxy_audio_decoder_cb, this );
            MLOG_TRACE( "RTSP Proxy ( suffix:%s ) init audio decoder", m_p_config->suffix );
        }
        if( !m_p_audio_encoder ) {
            m_p_audio_encoder = new CAudioEncoder;
            if( NULL == m_p_audio_encoder ) {
                MLOG_ERROR( "%s() => CAM[%s], new audio encoder failed", __func__, m_p_config->suffix );
                m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }

            AudioEncoderParam params;
            memset( &params, 0, sizeof( params ) );

            params.SrcSamplerate = m_a_samplerate;
            params.SrcChannels   = m_a_channels;
            params.SrcSamplefmt  = AV_SAMPLE_FMT_S16;
            params.DstCodec      = m_p_config->output.a_info.codec;
            params.DstSamplerate = m_p_config->output.a_info.samplerate;
            params.DstChannels   = m_p_config->output.a_info.channels;
            params.DstSamplefmt  = AV_SAMPLE_FMT_S16;
            params.DstBitrate    = m_p_config->output.a_info.bitrate;

            if( FALSE == m_p_audio_encoder->init( &params ) ) {
                MLOG_ERROR( "%s() => CAM[%s], audio encoder init failed", __func__, m_p_config->suffix );
                m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;
                return PROXY_RECODEC_NOT_NEED;
            }
            m_p_audio_encoder->addCallback( rtsp_proxy_audio_encoder_cb, this );
            MLOG_TRACE( "RTSP Proxy ( suffix:%s ) init audio encoder", m_p_config->suffix );
        }
        m_n_audio_recodec = PROXY_RECODEC_NEED;
    }
    else
        m_n_audio_recodec = PROXY_RECODEC_NOT_NEED;

    return m_n_audio_recodec;
}

static inline bool is_start_code_at(const uint8_t* data, int i, int len, int& offset)
{
    if( i + 4 < len )
    {
        if( data[i] == 0 && data[i+1] == 0 )
        {
            if( data[i+2] == 1 )
            {
                offset = 3;
                return true;
            }
            else if( data[i+2] == 0 && data[i+3] == 1 )
            {
                offset = 4;
                return true;
            }
        }
    }
    return false;
}

// =========================
// SPS / PPS 감지
// =========================

bool contains_sps_pps_h264(const uint8_t* data, int len)
{
    for( int i = 0, offset = 0; i + 4 < len; ++i ){
        if( is_start_code_at(data, i, len, offset) && i + offset < len ){
            uint8_t nal_type = data[i + offset] & 0x1F;
            if( nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS )
                return true;
        }
    }
    return false;
}

bool contains_sps_pps_h265(const uint8_t* data, int len)
{
    for( int i = 0, offset = 0; i + 5 < len; ++i ){
        if( is_start_code_at(data, i, len, offset) && i + offset < len ){
            uint8_t nal_header = data[i + offset];
            uint8_t nal_type = (nal_header >> 1) & 0x3F;
            if( nal_type >= HEVC_NAL_VPS && nal_type <= HEVC_NAL_PPS )
                return true;
        }
    }
    return false;
}

bool contains_sps_pps_mpeg4(const uint8_t* data, int len)
{
    for( int i = 0; i + 4 < len; ++i ){
        if( data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01 ){
            if( data[i+3] == MPEG4_VO_SEQUENCE || data[i+3] == MPEG4_VO )
                return true;
        }
    }
    return false;
}

bool CRtspProxy::containsSPSorPPS(const uint8_t* data, int len)
{
    switch( m_v_codec )
    {
    case VIDEO_CODEC_H264: return contains_sps_pps_h264 (data, len);
    case VIDEO_CODEC_H265: return contains_sps_pps_h265 (data, len);
    case VIDEO_CODEC_MP4:  return contains_sps_pps_mpeg4(data, len);
    default:               return false;
    }
}

// =========================
// Keyframe 감지
// =========================

bool is_keyframe_h264(const uint8_t* data, int len)
{
    for( int i = 0; i + 4 < len; ++i ){
        if( data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 ){
            uint8_t nal_type = data[i+3] & 0x1F;
            if( nal_type == H264_NAL_IDR )
                return true;
        }
    }
    return false;

}

bool is_keyframe_h265(const uint8_t* data, int len)
{
    const int H265_IDR_MIN = HEVC_NAL_BLA_W_LP;
    const int H265_IDR_MAX = HEVC_NAL_CRA_NUT;

    for( int i = 0, offset = 0; i + 5 < len; ++i ){
        if( is_start_code_at(data, i, len, offset) ){
            uint8_t nal_header = data[i + offset];
            uint8_t nal_type = (nal_header >> 1) & 0x3F;
            if( nal_type >= H265_IDR_MIN && nal_type <= H265_IDR_MAX )
                return true;
        }
    }
    return false;
}

bool is_keyframe_mpeg4(const uint8_t* data, int len)
{
    for( int i = 0; i + 5 < len; ++i ){
        if( data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 && data[i+3] == MPEG4_VOP ){
            uint8_t vop_type = (data[i+4] >> 6) & 0x03;
            if( vop_type == 0 ) // I-VOP
                return true;
        }
    }
    return false;
}

bool CRtspProxy::isKeyFrame(const uint8_t* data, int len)
{
	switch( m_v_codec )
	{
	case VIDEO_CODEC_H264: return is_keyframe_h264 (data, len);
	case VIDEO_CODEC_H265: return is_keyframe_h265 (data, len);
	case VIDEO_CODEC_MP4:  return is_keyframe_mpeg4(data, len);
    case VIDEO_CODEC_JPEG: return true;
	default:               return false;
	}
}

const BOOL CRtspProxy::videoDataDecode( uint8* p_data, int len )
{
    if( !m_p_video_decoder )
        return FALSE;

    try {
        const int proxy_status = m_init_regist_decoded_frame_queue.load();
        bool bShouldQueue = false;

        switch( proxy_status )
        {
        case ProxyTossRunnable:
            // (1) 이미 실행 중이면 모든 패킷을 큐잉 시도
            bShouldQueue = true;
            break;

        case ProxyTossNotSet:
        case ProxyTossInitialized:
            // (2) 디코더가 초기화되었지만 (NotSet) 또는 큐만 연결되었지만 (Initialized)
            //     아직 첫 키프레임을 받지 못한 상태.
            //     -> 오직 키프레임만 큐에 넣는다.
            if( isKeyFrame(p_data, len) == true )
            {
                bShouldQueue = true;

                // (3) 큐가 연결된 상태(Initialized)에서 키프레임을 받으면
                //     이제 Runnable 상태로 전환.
                if (proxy_status == ProxyTossInitialized)
                {
                    m_init_regist_decoded_frame_queue.store(ProxyTossRunnable);
                }
            }
            // else: (SPS/PPS 또는 P-Frame이면 bShouldQueue는 false 유지 -> 버려짐)
            break;

        default:
            // 알 수 없는 상태 (방어 코드)
            bShouldQueue = false;
            break;
        }

        // 큐에 넣지 않기로 결정되었으면 함수 종료
        if( !bShouldQueue )
        {
            return TRUE; // 오류는 아니므로 TRUE 반환 (패킷 처리 완료로 간주)
        }

        if( !m_packets_queue_ ){
            return FALSE;
        }

        const bool& is_realtime_mode = m_skip_if_already_exist_avframe_in_queue;
        const bool  is_over_q_size   = m_packets_queue_->size() > m_current_decode_q_max_size;

        // --- 백프레셔(Back-Pressure) 제어 ---
        if( is_realtime_mode && is_over_q_size )
        {
            if( isKeyFrame(p_data, len) == false )
            {
                // Case A: Key Frame이 아님 (P/B 프레임)
                // 디코더가 어차피 처리 못함. 즉시 폐기.
                MLOG_WARN("CAM<%s> Packet queue full (%d > %d). Dropping non-key frame.",
                    m_p_config->suffix, m_packets_queue_->size(), m_current_decode_q_max_size );

                if( m_current_decode_q_max_size < MAX_DECODE_PACKET_Q_SIZE_MAXIMUM ){
                    MLOG_WARN("CAM<%s> Packet queue maximum size : %d -> %d",
                        m_p_config->suffix, m_current_decode_q_max_size,
                        m_current_decode_q_max_size + MAX_DECODE_PACKET_Q_SIZE_DEFAULT
                    );
                    m_current_decode_q_max_size += MAX_DECODE_PACKET_Q_SIZE_DEFAULT;
                }
                return FALSE; // 패킷 폐기
            }
            else
            {
                // Case B: Key Frame (또는 MJPEG) 도착
                // 큐에 쌓인 모든 오래된 패킷을 비우고
                // 디코더가 이 최신 Key Frame부터 다시 시작하도록 함.
                MLOG_WARN("CAM<%s> Packet queue full (%d). Dropping all old frames "
                          "and resetting with new key frame.", m_p_config->suffix, m_packets_queue_->size() );

                m_packets_queue_->clear_with_action(
                    [](AVPacket* pkt) {
                        if (pkt) av_packet_free( &pkt );
                    }
                );
                // 큐를 비운 후, 아래 로직을 계속 진행하여 이 Key Frame을 큐에 추가.
            }
        }

        // AVPacket 구조체 할당
        AVPacket* packet = av_packet_alloc();
        if( !packet ) {
            MLOG_ERROR("CAM<%s> %s, av_packet_alloc() failed\r\n", m_p_config->suffix, __FUNCTION__);
            return FALSE;
        }

        // 3-2. AVPacket 내부에 새 버퍼를 할당 (데이터 소유)
        if( av_new_packet(packet, len) < 0 ){
            MLOG_ERROR("CAM<%s> %s, av_new_packet() failed for size %d\r\n",
                m_p_config->suffix, __FUNCTION__, len);
            av_packet_free(&packet);
            return FALSE;
        }

        // 3-3. 원본 데이터를 새 버퍼로 복사 (memcpy)
        memcpy( packet->data, p_data, len );

        // Feed the packet to the recorder before moving it to the decode queue
        if( isKeyFrame( p_data, len ) == true ){
            packet->flags |= AV_PKT_FLAG_KEY;
        }
        if( m_recorder && is_realtime_mode ) {
            m_recorder->pushVideoPacket( packet );
        }

        // 3-4. 큐에 삽입 (lvalue 'packet'의 소유권을 큐로 이동)
        m_packets_queue_->enqueue_move( std::move( packet ) );

        return TRUE; // 큐 삽입 성공
    }
    catch (...) {
        return FALSE;
    }
}

const BOOL CRtspProxy::videoDataEncode( AVFrame* frame )
{
    if( m_p_video_encoder ) {
        return m_p_video_encoder->encode( frame );
    }
    return FALSE;
}

const BOOL CRtspProxy::audioDataDecode( uint8* p_data, int len )
{
    if( m_p_audio_decoder ) {
        return m_p_audio_decoder->decode( p_data, len );
    }
    return FALSE;
}

const BOOL CRtspProxy::audioDataEncode( AVFrame* frame )
{
    if( m_p_audio_encoder ) {
        return m_p_audio_encoder->encode( frame );
    }
    return FALSE;
}

/*
---------------------------------------------------
# 처리 방식 : 실시간 처리 및 과잉 프레임 스킵
skip_if_already_exist_frame_in_q : TRUE
skip_coefficient : 초당 스킵하지 않고 처리할 수 있는 최대 프레임 카운트 ( count )
---------------------------------------------------
# 처리 방식 : 모든 프레임 체크 및 N배 벌크 프레임 걸러내기
skip_if_already_exist_frame_in_q : FALSE
skip_coefficient : 디코딩 된 프레임 중 몇 배수분의 1( 1/N )프레임만 실제로 처리할지의 배수
---------------------------------------------------

---------------------------------------------------
# Processing method: Real-time processing and skipping of excess frames
skip_if_already_exist_frame_in_q : TRUE
skip_frame_coefficient : The maximum number of frames that can be processed without skipping per second ( count )
---------------------------------------------------
# Processing method: Check all frames and filter out N-fold bulk frames
skip_if_already_exist_frame_in_q : FALSE
skip_frame_coefficient : A multiple of how many times ( 1/N ) of decoded frames will actually be processed
---------------------------------------------------
*/
bool CRtspProxy::setDecodedFrameSafeQueue
(
    sptrSafeQueue<std::shared_ptr<AVFrame>> sq,
    const bool skip_if_already_exist_frame_in_q,
    const int  skip_frame_coefficient
)
{
    if( m_init_regist_decoded_frame_queue.load() == ProxyTossNotSet ) {
        m_decoded_avframes                       = sq;
        m_skip_if_already_exist_avframe_in_queue = skip_if_already_exist_frame_in_q;
        m_skip_frame_coefficient                 = skip_frame_coefficient;
        m_init_regist_decoded_frame_queue.store( ProxyTossInitialized );

        return true;
    }
    if( sq == nullptr ){
        m_ignore_avframe_insert_before_terminate.store( true );
        MLOG_INFO("[%s] AVFrame Queue Unlinked", m_p_config->suffix);

        return true;
    }

    return false;
}

void CRtspProxy::avframeTossCallback( AVFrame* frame, const int type )
{
    // 최소 유효성 검사 (최대한 가볍게)
    if( !frame || !frame->data[0] || type != DATA_TYPE_IMAGE || !m_decoded_avframes ) {
        if( frame ) av_frame_free( &frame );
        return;
    }

    // 종료 대기 중인지 체크
    if( m_ignore_avframe_insert_before_terminate.load( std::memory_order_relaxed ) ) {
        av_frame_free( &frame );
        return;
    }

    const auto current_time = std::chrono::high_resolution_clock::now();

    // --- CASE A: 실시간 처리 및 과잉 프레임 스킵 (TRUE) ---
    if( m_skip_if_already_exist_avframe_in_queue )
    {
        if( m_decoded_avframes->size() > 0 ){
            av_frame_free( &frame );
            return;
        }

        const int& limit_fps = m_skip_frame_coefficient;
        if( limit_fps < 60 )
        {
            const int intervals = 1000 / limit_fps;
            auto      timelapse = std::chrono::duration_cast<std::chrono::milliseconds>( current_time - m_last_avframe_queue_insert_time );

            if( timelapse.count() < intervals ){
                av_frame_free( &frame );
                return;
            }
        }
        // 통과 시 아래의 큐 삽입 로직으로 이동
    }
    // --- CASE B: 모든 프레임 체크 및 N배수 필터링 (FALSE) ---
    else
    {
        const int& mod_limit = m_skip_frame_coefficient;
        size_t     frame_idx = inserted_frame.fetch_add( 1, std::memory_order_relaxed );

        if( frame_idx == 0 ){
            init_time = current_time;
        }

        if( mod_limit > 1 && ( frame_idx % mod_limit != 0 ) ){
            av_frame_free( &frame );
            return;
        }
        // 통과 시 아래의 큐 삽입 로직으로 이동
    }

    // 해상도 변경 감지 (Lock-free Read 우선)
    if( frame->width != m_v_width || frame->height != m_v_height )
    {
        std::lock_guard<std::mutex> lck( m_recv_video_mtx );

        if( frame->width != m_v_width || frame->height != m_v_height )
        {
            MLOG_WARN("Res Change: CAM[%s] %dx%d -> %dx%d", m_p_config->suffix, m_v_width, m_v_height, frame->width, frame->height);
            m_v_width  = frame->width;
            m_v_height = frame->height;
        }
    }

    // 큐 삽입 및 시간 업데이트
    m_last_avframe_queue_insert_time = current_time;

    // 큐 삽입 직전에만 shared_ptr 생성
    auto avframe_deleter = []( AVFrame* f ){ if( f ) av_frame_free( &f ); };
    std::shared_ptr<AVFrame> sptr_avframe { frame, avframe_deleter };

    m_decoded_avframes->enqueue_move( std::move( sptr_avframe ) );

    // 프레임 인덱스 업데이트 및 로그 출력 (모든 프레임 체크 모드에서만)
    if( m_skip_if_already_exist_avframe_in_queue == false )
    {
        size_t log_idx = inserted_frame.load( std::memory_order_relaxed );
        if( log_idx > 100 && log_idx % 100 == 1 )
        {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>( current_time - init_time ).count();
            duration = ( duration == 0 ) ? 1 : duration;

            MLOG_INFO("[%s] AVFrame Queue size = %4d ( %5.2f fps ) ( %5d frame / %4d sec )",
                m_p_config->suffix, m_decoded_avframes->size(),
                log_idx / (float)(duration), log_idx - 1, duration
            );
        }
    }
}

ProxyVideoInfo CRtspProxy::getProxyVideoInfo( void )
{
    // 락을 사용하여 스레드 안전하게 변수들을 읽습니다.
    std::unique_lock<std::mutex> lck( m_recv_video_mtx );

    ProxyVideoInfo info {
        .proxy_id     = m_proxy_id,
        .v_width      = m_v_width,
        .v_height     = m_v_height,
        .original_url = std::string { m_url }
    };

    lck.unlock();

    return info;
}

void CRtspProxy::freeConn()
{
    if( m_rtsp ){
        // stop rtsp connection
        m_rtsp->rtsp_stop();
        m_rtsp->rtsp_close();

        delete m_rtsp;
        m_rtsp = NULL;
    }

    if( m_mjpeg ){
        delete m_mjpeg;
        m_mjpeg = NULL;
    }
}

const bool CRtspProxy::reconnectAfterFreeConnect()
{
    if( check_url_is_rtsp_type( m_url ) == true ){
        char url[500] = { '\0', };

        if( m_redirect_url[0] == 0 )
            strncpy( url, m_url, sizeof( url ) - 1 );
        else {
            strncpy( url, m_redirect_url, sizeof( url ) - 1 );
            m_redirect_url[0] = 0;
        }

        m_rtsp = new CRtsp;
        if( NULL == m_rtsp ){
            MLOG_ERROR( "%s() => New RTSP Failed ( url: %s )", __func__, url );
            return false;
        }
        m_rtsp->set_notify_cb  ( event_notify_cb, this );
        m_rtsp->set_video_cb   ( rtsp_video_cb    );
        m_rtsp->set_audio_cb   ( rtsp_audio_cb    );
        m_rtsp->set_redirect_cb( rtsp_redirect_cb );

        const bool restart_status = m_rtsp->rtsp_start( url, m_user, m_pass );
        if( restart_status == false )
            MLOG_ERROR( "Camera [%s] Restart Failed", m_p_config->suffix );
        return restart_status;
    }
    else if( check_url_is_http_s_type( m_url ) == true ) {

        m_mjpeg = new CHttpMjpeg;
        if( NULL == m_mjpeg ){
            MLOG_ERROR( "%s() => new http-mjpeg failed ( url: %s )", __func__, m_url );
            return false;
        }
        m_mjpeg->set_notify_cb( event_notify_cb, this );
        m_mjpeg->set_video_cb ( jpeg_video_cb );

        const bool restart_status = m_mjpeg->mjpeg_start( m_url, m_user, m_pass );
        if( restart_status == false )
            MLOG_ERROR( "Camera [%s] Restart Failed", m_p_config->suffix );
        return restart_status;
    }
    else {
        MLOG_TRACE( "RTSP URL is neither RTSP or HTTP" );
        MLOG_TRACE( "VMS NAME : %s", m_p_config->vmsName );
    }
    return FALSE;
}

const bool CRtspProxy::reconnectWithDelay( const int delay_sec )
{
    MLOG_INFO( "Camera<%s> { %s } connection terminated and will attempt to reconnect in %d seconds.",
               m_p_config->suffix, m_url, delay_sec );

    freeConn();

    if( delay_sec > 0 )
        std::this_thread::sleep_for( std::chrono::seconds( delay_sec ) );

    return this->reconnectAfterFreeConnect();
}

const bool CRtspProxy::restartConn()
{
	return this->reconnectWithDelay( 0 );
}

void CRtspProxy::clearNotify()
{
    int evt = 0;
    m_notify_queue->clear_with_action(
        [](int evt_dq) {
            UNUSE_PARAM(evt_dq);
        }
    );
}

const int CRtspProxy::parseVideoSize( uint8* p_data, int len )
{
    int return_value = -1;
    uint8 nalu_t;

    if( p_data == NULL || len < 5 )
        return return_value;

    if( VIDEO_CODEC_H264 == m_v_codec ) {
        int s_len = 0, n_len = 0, parse_len = len;

        uint8* p_cur = p_data;
        while( p_cur ) {
            uint8* p_next = avc_split_nalu( p_cur, parse_len, &s_len, &n_len );
            if( n_len < 5 )
                return 0;

            nalu_t = ( p_cur[s_len] & 0x1F );

            nal_t nal {};
            nal.i_payload = n_len - s_len - 1;
            nal.p_payload = p_cur + s_len + 1;
            nal.i_type = nalu_t;

            int b_start;
            if( nalu_t == H264_NAL_SPS ) {
                h264_t parse;
                h264_parser_init( &parse );
                h264_parser_parse( &parse, &nal, &b_start );

                m_v_width = parse.i_width;
                m_v_height = parse.i_height;
                MLOG_TRACE( "%s() => CAM[%s], H264 / width[%d] x height[%d]", __func__, m_p_config->suffix, m_v_width, m_v_height );

                return 0;
            }
            parse_len -= n_len;
            p_cur = p_next;
        }
    } // VIDEO_CODEC_H264

    else if( VIDEO_CODEC_H265 == m_v_codec ) {
        int s_len, n_len = 0, parse_len = len;

        uint8* p_cur = p_data;
        while( p_cur ) {
            uint8* p_next = avc_split_nalu( p_cur, parse_len, &s_len, &n_len );
            if( n_len < 5 )
                return 0;

            nalu_t = ( p_cur[s_len] >> 1 ) & 0x3F;
            if( nalu_t == HEVC_NAL_SPS ) {
                h265_t parse;
                h265_parser_init( &parse );

                if( h265_parser_parse( &parse, p_cur + 4, n_len - s_len ) == 0 ) {

                    m_v_width = parse.pic_width_in_luma_samples;
                    m_v_height = parse.pic_height_in_luma_samples;
                    MLOG_TRACE( "%s() => CAM[%s], H265 / width[%d] x height[%d]", __func__, m_p_config->suffix, m_v_width, m_v_height );

                    return 0;
                }
            }
            parse_len -= n_len;
            p_cur = p_next;
        }
    } // VIDEO_CODEC_H265

    else if( VIDEO_CODEC_JPEG == m_v_codec ) {
        int offset = 0;
        int size_chunk = 0;

        while( offset < len - 8 && p_data[offset] == 0xFF ) {
            if( p_data[offset + 1] == MARKER_SOF0 ) {
                int h = ( ( p_data[offset + 5] << 8 ) | p_data[offset + 6] );
                int w = ( ( p_data[offset + 7] << 8 ) | p_data[offset + 8] );

                m_v_width = w;
                m_v_height = h;
                MLOG_TRACE( "%s() => CAM[%s], MJPEG / width[%d] x height[%d]", __func__, m_p_config->suffix, m_v_width, m_v_height );

                return_value = 0;
                break;
            }
            else if( p_data[offset + 1] == MARKER_SOI ) {
                offset += 2;
            }
            else {
                size_chunk = ( ( p_data[offset + 2] << 8 ) | p_data[offset + 3] );
                offset += 2 + size_chunk;
            }
        }
    } // VIDEO_CODEC_JPEG

    else if( VIDEO_CODEC_MP4 == m_v_codec ) {
        int pos = 0;
        int vol_f = 0;
        int vol_pos = 0;
        int vol_len = 0;

        while( pos < len - 4 ) {
            if( p_data[pos] == 0 && p_data[pos + 1] == 0 && p_data[pos + 2] == 1 ){
                if( p_data[pos + 3] >= 0x20 && p_data[pos + 3] <= 0x2F ) {
                    vol_f = 1;
                    vol_pos = pos + 4;
                }
                else if( vol_f ) {
                    vol_len = pos - vol_pos;
                    break;
                }
            }
            pos++;
        }

        if( !vol_f )
            return 0;
        else if( vol_len <= 0 )
            vol_len = len - vol_pos;

        int vo_ver_id = 0;
        BitVector bv( &p_data[vol_pos], 0, vol_len * 8 );

        bv.skipBits( 1 );                /* random access */
        bv.skipBits( 8 );                /* vo_type */

        if( bv.get1Bit() ) {             /* is_ol_id */
            vo_ver_id = bv.getBits( 4 ); /* vo_ver_id */
            bv.skipBits( 3 );			 /* vo_priority */
        }

        if( bv.getBits( 4 ) == 15 )	{	 /* aspect_ratio_info */
            bv.skipBits( 8 );			 /* par_width */
            bv.skipBits( 8 );			 /* par_height */
        }

        if( bv.get1Bit() ) {             /* vol control parameter */
            int chroma_format = bv.getBits( 2 );

            bv.skipBits( 1 );		     /* low_delay */
            if( bv.get1Bit() ) {
                /* vbv parameters */
                bv.getBits( 15 );		 /* first_half_bitrate */
                bv.skipBits( 1 );
                bv.getBits( 15 );		 /* latter_half_bitrate */
                bv.skipBits( 1 );
                bv.getBits( 15 );		 /* first_half_vbv_buffer_size */
                bv.skipBits( 1 );
                bv.getBits( 3 );		 /* latter_half_vbv_buffer_size */
                bv.getBits( 11 );		 /* first_half_vbv_occupancy */
                bv.skipBits( 1 );
                bv.getBits( 15 );		 /* latter_half_vbv_occupancy */
                bv.skipBits( 1 );
            }
        }

        int shape = bv.getBits( 2 ); /* vol shape */
        if( shape == 3 && vo_ver_id != 1 )
            bv.skipBits( 4 );  /* video_object_layer_shape_extension */

        bv.skipBits( 1 );

        int framerate = bv.getBits( 16 );
        int time_increment_bits = ( int ) ( log( framerate - 1.0 ) * 1.44269504088896340736 + 1 ); // log2(framerate - 1) + 1
        if( time_increment_bits < 1 )
            time_increment_bits = 1;

        bv.skipBits( 1 );
        if( bv.get1Bit() != 0 )     /* fixed_vop_rate  */
            bv.skipBits( time_increment_bits );

        if( shape != 2 ){
            if( shape == 0 ) {
                bv.skipBits( 1 );
                int w = bv.getBits( 13 );

                bv.skipBits( 1 );
                int h = bv.getBits( 13 );

                m_v_width = w;
                m_v_height = h;
                MLOG_TRACE( "%s() => CAM[%s], MPEG4 / width[%d] x height[%d]", __func__, m_p_config->suffix, m_v_width, m_v_height );
                return_value = 0;
            }
        }
    } // MPEG4

    return return_value;
}

/**
 * @brief Public interface to start recording via proxy.
 */
bool CRtspProxy::startProxyRecording( const std::string& file_name, const int pre_roll_sec, const int post_roll_sec )
{
    if( m_recorder ) {
        return m_recorder->startRecording( file_name, pre_roll_sec, post_roll_sec );
    }
    return false;
}

std::optional<double> CRtspProxy::getRealtimeFps() const
{
    if( m_recorder ) {
        return m_recorder->getRealtimeFps();
    }
    return std::nullopt;
}
