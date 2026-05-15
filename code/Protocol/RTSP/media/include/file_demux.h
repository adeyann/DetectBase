/***************************************************************************************
*
* IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
*
* By downloading, copying, installing or using the software you agree to this license.
* If you do not agree to this license, do not download, install,
* copy or use the software.
*
* Copyright (C) 2014-2019, Happytimesoft Corporation, all rights reserved.
*
* Redistribution and use in binary forms, with or without modification, are permitted.
*
* Unless required by applicable law or agreed to in writing, software distributed
* under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
* CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
* language governing permissions and limitations under the License.
*
****************************************************************************************/

#ifndef _FILE_DEMUX_H
#define _FILE_DEMUX_H

#include "audio_encoder.h"
#include "video_encoder.h"

#include <stdint.h>

// extern "C" 블록으로 FFmpeg C 라이브러리 헤더를 감쌉니다.
extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/intreadwrite.h>
    #include <libavutil/avstring.h>
    #include <libavutil/base64.h>
    #include <libavutil/channel_layout.h> // 최신 채널 API를 위해 추가
    // #include <libavcodec/bsf.h>
}

// 원본의 사용자 정의 타입을 그대로 사용합니다.
typedef int      BOOL;
typedef uint8_t  uint8;
typedef uint32_t uint32;
typedef int64_t  int64;

/**
* callback function prototype
*
* @param data the encoded data
* @param size the data size
* @param type DATA_TYPE_AUDIO or DATA_TYPE_VIDEO
* @param nbsamples audio sample numbers
* @param pUserdata user data
*/
typedef void (*DemuxCallback)(uint8 * data, int size, int type, int nbsamples, BOOL waitnext, void * pUserdata);

class CFileDemux
{
public:
    CFileDemux(const char * filename);
    ~CFileDemux();

    /**
    * Is there an audio stream?
    *
    * @return TURE has audio stream, FALSE hasn't audio stream
    */
    BOOL    hasAudio() {return m_nAudioIndex != -1;}
    /**
    * Is there an video stream?
    *
    * @return TURE has video stream, FALSE hasn't video stream
    */
    BOOL    hasVideo() {return m_nVideoIndex != -1;}

    /**
    * get video width
    *
    * @return if has video stream, the video width, or 0
    */
    int     getWidth();
    /**
    * get video height
    *
    * @return if has video stream, the video height, or 0
    */
    int     getHeight();
    /**
    * get video frame rate
    *
    * @return if has video stream, the video frame rate, or 0
    */
    int     getFramerate();
    /**
    * get audio sample rate
    *
    * @return if has audio stream, the audio sample rate, or 0
    */
    int     getSamplerate();
    /**
    * get audio channels numbers
    *
    * @return if has audio stream, the audio channels numbers, or 0
    */
    int     getChannels();

    /**
    * get stream duration
    *
    * @return the stream duration, unit is millisecond
    */
    int64   getDuration() {return m_nDuration;}

    /**
    * get stream current position
    *
    * @return the stream current position, unit is millisecond
    */
    int64   getCurPos() {return m_nCurPos;}

    /**
    * set audio output format
    *
    * @param codec audio output codec, see media_format.h
    * @param samplerate audio output samplerate
    * @param channels audio output channels
    * @return TRUE on success, FALSE on error
    */
    BOOL    setAudioFormat(int codec, int samplerate, int channels, int bitrate);
    /**
    * set video output format
    *
    * @param codec video output codec, see media_format.h
    * @param width video output width
    * @param height video output height
    * @param framerate video output framerate
    * @return TRUE on success, FALSE on error
    */
    BOOL    setVideoFormat(int codec, int width, int height, int framerate, int bitrate);
    /**
    * set data callback function
    *
    * @param pCallback callback functon pointer
    * @param pUserdata user data
    */
    void    setCallback(DemuxCallback pCallback, void * pUserdata);

    /**
    * read a frame, data will be transmitted to the user by callback
    *
    * @return TRUE on success, FALSE on error
    */
    BOOL    readFrame();
    /**
    * get video sdp line
    *
    * @param rtp_pt rtp payload type
    * @return sdp string on success, NULL on error
    */
    char* getVideoAuxSDPLine(int rtp_pt);
    /**
    * get audio sdp line
    *
    * @param rtp_pt rtp payload type
    * @return sdp string on success, NULL on error
    */
    char* getAudioAuxSDPLine(int rtp_pt);

    /**
    * seek stream
    *
    * @param pos seek position, unit is second
    * @return TRUE on success, FALSE on error
    */
    BOOL    seekStream(double pos);

    void    dataCallback(uint8 * data, int size, int type, int nbsamples, BOOL waitnext);

private:
    BOOL    init(const char * filename);
    void    uninit();

    void    flushVideo();
    void    flushAudio();

    // API 변경에 따라 AVPacket 포인터를 받도록 수정
    void    audioRecodec(AVPacket* pkt);
    void    videoRecodec(AVPacket* pkt);

    char* getH264AuxSDPLine(int rtp_pt);
    char* getMP4AuxSDPLine(int rtp_pt);
    char* getAACAuxSDPLine(int rtp_pt);
    char* getH265AuxSDPLine(int rtp_pt);

private:
    int                 m_nAudioIndex;      // audio stream index
    int                 m_nVideoIndex;      // video stream index
    int64               m_nDuration;        // the stream duration, unit is millisecond
    int64               m_nCurPos;          // the current play position, unit is millisecond

    int                 m_nNalLength;

    AVFormatContext* m_pFormatContext;   // format context
    AVCodecContext* m_pACodecCtx;       // audio codec context
    AVCodecContext* m_pVCodecCtx;       // video codec context
    AVFrame* m_pFrame;           // 디코딩을 위한 재사용 프레임 (성능 최적화)

    CVideoEncoder* m_pVideoEncoder;    // video encoder
    CAudioEncoder* m_pAudioEncoder;    // audio encoder

    BOOL                m_bFirst;
    DemuxCallback       m_pCallback;        // callback function pointer
    void* m_pUserdata;        // user data

    uint32              m_nLoopNums;        // current loop numbers
};

#endif // _FILE_DEMUX_H