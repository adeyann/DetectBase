#ifndef RTSP_PROXY_H
#define RTSP_PROXY_H

#include "MgenLogger.h"
#include "SafeQueue.h"
#include "linked_list.h"
#include "SafeThread.h"
#include "MgenTypes.h"

#include "rtsp_cln.h"
#include "rtsp_comm_cfg.h"
#include "http_mjpeg_cln.h"
#include "rtsp_proxy_recorder.h"

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
#include "video_decoder.h"
#include "audio_decoder.h"
#include "video_encoder.h"
#include "audio_encoder.h"
#endif

#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>
#include <optional>

extern "C" {
#include <libswscale/swscale.h>
}

// Define const
constexpr int    DECODE_GPU_NOT_USE = -1;
constexpr size_t MAX_DECODE_PACKET_Q_SIZE_DEFAULT = 64;
constexpr size_t MAX_DECODE_PACKET_Q_SIZE_MAXIMUM = MAX_DECODE_PACKET_Q_SIZE_DEFAULT * 16;

// Declare class
class CRtspProxy;

// Define Structure
typedef struct {
    uint32 has_output : 1;  // allocate 1 bit
    uint32 has_detect : 1;
    uint32 reserved   : 30; // allocate 30 bit  ___ total 32 bit

    char   suffix[100];
    char   url[256];
    char   user[32];
    char   pass[32];
    char   guid[255];
    char   ip[20];
    int    port;
    char   vmsName[255];
    char   group[255];

    RTSP_OUTPUT output;
} PROXY_CFG;

typedef struct _RPROX {
    struct _RPROX* next;
    PROXY_CFG      cfg;
    CRtspProxy*    proxy;
} RTSP_PROXY;

typedef void ( *ProxyDataCB )( uint8* data, int size, int type, void* pUserdata );
typedef struct {
    ProxyDataCB pCallback;
    void*       pUserdata;
    BOOL        bFirst;
} ProxyCB;

#ifdef __cplusplus
extern "C" {
#endif
    RTSP_PROXY* rtsp_add_proxy( RTSP_PROXY** p_proxy );
    void        rtsp_free_proxies( RTSP_PROXY** p_proxy );
    const int   rtsp_get_proxy_nums();
    RTSP_PROXY* rtsp_proxy_match( const char* suffix );
    void        rtsp_proxy_video_decoder_cb( AVFrame* frame, void* p_user );
#ifdef __cplusplus
}
#endif

enum _enum_PROXY_NEED_RECODEC
{
    PROXY_RECODEC_NEED           =  1,
    PROXY_RECODEC_STILL_NOT_SURE =  0,
    PROXY_RECODEC_NOT_NEED       = -1
};

enum _enum_PROXY_NEED_DETECT
{
    NEED_DETECT     =  1,
    NOT_SURE_DETECT =  0,
    NOT_NEED_DETECT = -1,
};

enum class ProxyAVFrameToss
{
    NotSure,
    NotNeed,
    Need,
};

enum ProxyTossQueueStatus
{
    ProxyTossNotSet      = 0,
    ProxyTossInitialized = 1,
    ProxyTossRunnable    = 2,
    ProxyTossDeleted     = 3,
};

struct ProxyVideoInfo
{
    int         proxy_id     = 0;
    int         v_width      = 0;
    int         v_height     = 0;
    std::string original_url = "";
};

using namespace std;
class CRtspProxy
{
// Constructor & Destructor
public:
    explicit CRtspProxy( PROXY_CFG* p_proxy_config ) noexcept;

    // for derived class : CRtspProxyDetect
    virtual ~CRtspProxy();

// Public Methods
public:
    // Initializer
    const bool startConn( const char* url, char* user, char* pass );

    // Event binder
    void onAudio( uint8* pdata, int len, uint32 ts, uint16 seq );
    void onVideo( uint8* pdata, int len, uint32 ts, uint16 seq );
    void onNotify( int evt );

    // Callback Functions
    void addCallback ( ProxyDataCB pCallback, void* pUserdata );
    void delCallback ( ProxyDataCB pCallback, void* pUserdata );
    void runCallBacks( uint8* data, int size, int type );

    // Gets
    char*     getVideoAuxSDPLine( const int rtp_pt );
    char*     getAudioAuxSDPLine( const int rtp_pt );
    const int getRtspRuaState() { return m_rtsp->get_rua_state(); }

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    const int  getVideoRecodec() { return m_n_video_recodec; }
    const int  getAudioRecodec() { return m_n_audio_recodec; }

    const int  needVideoRecodec( uint8* p_data, int len );
    const int  needAudioRecodec();

    const BOOL videoDataDecode( uint8* p_data, int len );
    const BOOL videoDataEncode( AVFrame* frame );
    const BOOL audioDataDecode( uint8* p_data, int len );
    const BOOL audioDataEncode( AVFrame* frame );
#endif

    /**
     * @brief Triggers a video recording task for this proxy.
     * @param file_name Output file path.
     * @param pre_roll_sec Pre-event duration (seconds).
     * @param post_roll_sec Post-event duration (seconds).
     * @return true if the recording task is successfully initiated.
     */
    bool startProxyRecording( const std::string& file_name, const int pre_roll_sec, const int post_roll_sec );

    /**
     * @brief Retrieves the current real-time FPS based on incoming packets.
     * Calculates an exponentially weighted moving average (EMA) to smooth out fluctuations.
     * @return Optional double representing the current FPS, or std::nullopt if insufficient data.
     */
    std::optional<double> getRealtimeFps( void ) const;

    //
    inline int getProxyID( void ) { return this->m_proxy_id; }
    ProxyVideoInfo getProxyVideoInfo( void );

    // For decoded frame internal queueing
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
    bool setDecodedFrameSafeQueue(
        sptrSafeQueue<std::shared_ptr<AVFrame>> sq,
        const bool skip_if_already_exist_frame_in_q = true,
        const int  skip_frame_coefficient = 30 );

    bool containsSPSorPPS(const uint8_t* data, int len);
    bool isKeyFrame(const uint8_t* data, int len);

    void avframeTossCallback( AVFrame* frame, const int type );
    ProxyAVFrameToss needAVFrameToss( uint8* p_data, int len );

// Private Methods
private:
    void       freeConn();
    const bool reconnectAfterFreeConnect();

	const bool reconnectWithDelay( const int delay_sec );
    const bool restartConn();

    void       clearNotify();

    void NotifyThreadRunner( void );
    void NotifyThreadCloser( void );

    void DecodeThreadRunner( void );
    void DecodeThreadCloser( void );

    const bool isCallbackExist( ProxyDataCB pCallback, void* pUserdata );
    const int  parseVideoSize( uint8* pdata, int len );
    char*      getH264AuxSDPLine( const int rtp_pt );
    char*      getH265AuxSDPLine( const int rtp_pt );
    char*      getMP4AuxSDPLine ( const int rtp_pt );
    char*      getAACAuxSDPLine ( const int rtp_pt );

protected:
    void setInstanceTerminate( const bool b_terminate )
    {
        m_is_terminate = b_terminate;
    }

// Public variables
public:
    uint32 m_has_video : 1;
    uint32 m_has_audio : 1;
    uint32 m_inited    : 1;
    uint32 m_reserved  : 29;

    PROXY_CFG* m_p_config;

    int m_v_codec;
    int m_v_width;
    int m_v_height;

    int m_a_codec;
    int m_a_samplerate;
    int m_a_channels;

    char m_url[500];
    char m_redirect_url[500];

protected:
    char m_user[32];
    char m_pass[32];

    CRtsp*      m_rtsp;
    CHttpMjpeg* m_mjpeg;

    shared_ptr<SafeQueue<int>> m_notify_queue;

    mutex        m_p_callback_mtx;
    LINKED_LIST* m_p_callback_list;

    mutex m_recv_video_mtx;
    bool  m_recv_video;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int            m_n_video_recodec;
    int            m_n_audio_recodec;
    CVideoDecoder* m_p_video_decoder;
    CAudioDecoder* m_p_audio_decoder;
    CVideoEncoder* m_p_video_encoder;
    CAudioEncoder* m_p_audio_encoder;
#endif

    // Recording Component
    std::unique_ptr<CRtspProxyRecorder> m_recorder; // Recorder instance for this proxy

    int  m_proxy_id = 0;
    bool m_is_terminate;

    // Avframe Toss : flag | configs
    ProxyAVFrameToss m_n_avframe_toss = ProxyAVFrameToss::NotSure;
    int    m_skip_frame_coefficient = 30;
    size_t m_avframe_queue_warn_threshold_count = 100;
    bool   m_skip_if_already_exist_avframe_in_queue = true;

    // Avframe Toss : Queue
    sptrSafeQueue<std::shared_ptr<AVFrame>> m_decoded_avframes = nullptr;
    std::atomic<bool> m_ignore_avframe_insert_before_terminate { false };
    std::atomic<int>  m_init_regist_decoded_frame_queue { ProxyTossNotSet };

    // Avframe Toss : for frame limit
    std::chrono::time_point<std::chrono::system_clock> m_last_avframe_queue_insert_time;

    // Notify Thread
    MGEN::SafeThread        m_notify_thread_;
    mutable std::mutex      m_notify_thread_mtx_;
    std::condition_variable m_notify_thread_cond_;

    // Decoder Thread
    sptrSafeQueue<AVPacket*> m_packets_queue_ = nullptr;
    MGEN::SafeThread         m_decode_thread_;

    //
    MGEN::EventTime init_time;
    MGEN::EventTime last_time;
    std::atomic<unsigned int> inserted_frame { 0 };

    //
    size_t m_current_decode_q_max_size = MAX_DECODE_PACKET_Q_SIZE_DEFAULT * 4;

}; // cls: CRtspProxy
#endif