#ifndef RTSP_PROXY_RECORDER_H
#define RTSP_PROXY_RECORDER_H

#include "SafeThread.h"       // MGEN::SafeThread
#include "h264_parser.h"      // H264SPSInfo, H264PPSInfo, parse_sps, parse_pps

#include <string>             // std::string
#include <deque>              // std::deque
#include <list>               // std::list
#include <vector>             // std::vector
#include <mutex>              // std::mutex
#include <atomic>             // std::atomic
#include <condition_variable> // std::condition_variable
#include <memory>             // std::shared_ptr
#include <optional>           // std::optional

extern "C" {
#include <libavcodec/avcodec.h>   // AVPacket, AVCodecParameters
#include <libavformat/avformat.h> // AVFormatContext, AVStream
#include <libavutil/time.h>       // av_gettime
}

/**
 * @brief Represents a single video recording session.
 * Includes independent buffers to prevent data corruption during concurrent tasks.
 */
struct RecordingTask {
    // Basic Task Info
    std::string      fileName;        // Target output file path
    int64_t          targetDuration;  // Requested duration in microseconds
    int64_t          startSystemTime; // System time when recording was triggered
    int64_t          firstPTS;        // PTS of the first packet for normalization
    int64_t          lastWrittenPTS;  // Global buffer PTS tracker to avoid duplicates

    // State Flags
    bool             isPreRollDone;   // Flag for historical packets written
    bool             isClosing;       // Duration reached, waiting for next I-frame
    bool             isFinished;      // Task completion flag

    // FFmpeg Contexts
    AVFormatContext* fmtCtx;          // Output container context
    AVStream* outStream;       // Video stream reference

    // Per-Task Merging Buffers for Fragmented NAL units
    std::vector<uint8_t> mergeBuffer;     // Accumulates fragments (AUD/SEI/Slices)
    int64_t              lastPtsInTask;   // Last PTS processed for this specific task
    int                  lastFlagsInTask; // Accumulated flags (Keyframe, etc.)

    // 마이크로초 단위의 Pre-roll 시간
    int64_t preRollUs;

    RecordingTask()
        : fileName        ( "" )
        , targetDuration  ( 0 )
        , startSystemTime ( 0 )
        , firstPTS        ( AV_NOPTS_VALUE )
        , lastWrittenPTS  ( AV_NOPTS_VALUE )
        , isPreRollDone   ( false )
        , isClosing       ( false )
        , isFinished      ( false )
        , fmtCtx          ( nullptr )
        , outStream       ( nullptr )
        , lastPtsInTask   ( -1 )
        , lastFlagsInTask ( 0 )
        , preRollUs       ( 0 )
    {
        mergeBuffer.reserve( 1024 * 256 ); // Pre-reserve 256KB for a frame
    }
};

/**
 * @brief Handles video recording by maintaining a sliding window of packets.
 * Supports multi-tasking with independent fragment merging per task.
 */
class CRtspProxyRecorder {
public:
    /**
     * @brief Constructor
     * @param proxy_id Unique identifier for the associated camera stream.
     */
    explicit CRtspProxyRecorder( const int proxy_id ) noexcept;

    /**
     * @brief Destructor
     */
    ~CRtspProxyRecorder();

    /**
     * @brief Pushes a new video packet into the history buffer.
     * Also scans for SPS/PPS to ensure codec parameters are up-to-date.
     * @param pkt Original packet from the RTSP stream.
     */
    void pushVideoPacket( const AVPacket* pkt );

    /**
     * @brief Initiates a new recording task.
     * @param file_name Output file path (e.g., .mp4).
     * @param pre_roll_sec Seconds to include before the current time.
     * @param post_roll_sec Seconds to include after the current time.
     * @return true if the task is successfully registered.
     */
    bool startRecording( const std::string& file_name, const int pre_roll_sec, const int post_roll_sec );

    /**
     * @brief Updates codec parameters (SPS/PPS) for the recorder.
     * @param params Codec parameters from the decoder or RTSP stream.
     */
    void setVideoCodecParameters( const AVCodecParameters* params );

    /**
     * @brief Retrieves the current real-time FPS based on incoming packets.
     * Calculates an exponentially weighted moving average (EMA) to smooth out fluctuations.
     * @return Optional double representing the current FPS, or std::nullopt if insufficient data.
     */
    std::optional<double> getRealtimeFps( void ) const;

private:
    /**
     * @brief Worker thread function to process recording tasks asynchronously.
     * Triggered by MGEN::SafeThread runner.
     */
    void workerThreadRunner( void );

    /**
     * @brief Cleanup function called when the thread is stopped.
     * Triggered by MGEN::SafeThread closer.
     */
    void workerThreadCloser( void );

    /**
     * @brief Cleans up expired packets from the history buffer.
     * Keeps the buffer size within the pre-roll limit based on time.
     */
    void manageHistoryBuffer( void );

    /**
     * @brief Scans the packet for SPS/PPS NAL units and updates codec parameters.
     * Essential for cases where initial extradata is missing or changes mid-stream.
     * @param pkt The packet to analyze.
     */
    void updateParametersFromBitstream( const AVPacket* pkt );

    /**
     * @brief 병합된 프레임 버퍼를 FFmpeg Muxer를 통해 파일에 기록합니다.
     */
    void writeMergedPacketToDisk( std::shared_ptr<RecordingTask> task );

    /**
     * @brief Updates the real-time FPS calculation based on the PTS of incoming packets.
     * Uses an exponentially weighted moving average (EMA) to provide a stable FPS estimate.
     */
    void updateRealtimeFps( const int64_t current_pts );

private:
    int _proxy_id;

    // History Management
    std::mutex            _history_mtx;
    std::deque<AVPacket*> _history_buffer;
    int64_t               _max_history_ns;

    // Task Management
    std::mutex _task_mtx;
    std::list<std::shared_ptr<RecordingTask>> _active_tasks;

    // Metadata
    AVCodecParameters* _v_codec_params;
    H264SPSInfo        _current_sps;
    H264PPSInfo        _current_pps;

    // Raw NALs for self-init
    std::vector<uint8_t> _v_raw;
    std::vector<uint8_t> _s_raw;
    std::vector<uint8_t> _p_raw;

    // Threading
    MGEN::SafeThread        _writer_thread;
    std::condition_variable _cv;
    std::mutex              _cv_mtx;

    // 실시간 FPS 계산용 상태 변수 ( without mutex )
    std::atomic<double> _realtime_fps { 0.0 };

    // 파편화 보정용 상태 변수
    int64_t _last_pts_for_fps     { AV_NOPTS_VALUE };
    int64_t _interval_accumulator { 0 };
    int     _logical_frame_count  { 0 };

    // 윈도우 제어용
    std::chrono::steady_clock::time_point _window_start_tp {};
};

#endif // RTSP_PROXY_RECORDER_H