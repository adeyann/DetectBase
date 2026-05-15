/***************************************************************************************
 *
 *  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
 *
 *  By downloading, copying, installing or using the software you agree to this license.
 *  If you do not agree to this license, do not download, install, 
 *  copy or use the software.
 *
 *  Copyright (C) 2014-2019, Happytimesoft Corporation, all rights reserved.
 *
 *  Redistribution and use in binary forms, with or without modification, are permitted.
 *
 *  Unless required by applicable law or agreed to in writing, software distributed 
 *  under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 *  CONDITIONS OF ANY KIND, either express or implied. See the License for the specific
 *  language governing permissions and limitations under the License.
 *
****************************************************************************************/

#ifndef _VIDEO_ENCODER_H
#define _VIDEO_ENCODER_H

#include "linked_list.h"

extern "C" 
{
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/avstring.h>
#include <libavutil/base64.h>
#include <libavutil/imgutils.h>
}

typedef void (*VideoDataCallback)(uint8 *data, int size, void *pUserdata);

typedef struct
{
	VideoDataCallback pCallback;
	void *            pUserdata;
	BOOL              bFirst;
} VideoEncoderCB;

typedef struct
{
    int             SrcWidth;       // source video width
    int             SrcHeight;      // source video height
    AVPixelFormat   SrcPixFmt;      // source video pixel format
    int             Updown;         // 
    
    int             DstCodec;       // output video codec, see media_format.h
    int             DstWidth;       // output video width
    int             DstHeight;      // output video height
    int             DstFramerate;   // output video frame rate
    int             DstBitrate;     // output bitrate
} VideoEncoderParam;

class CVideoEncoder
{
public:
    CVideoEncoder();
    ~CVideoEncoder();

    static AVCodecID    toAVCodecID(int codec);
    static AVPixelFormat toAVPixelFormat(int pixfmt);

    BOOL                init(VideoEncoderParam * params);
    void                uninit();
    BOOL                encode(uint8 * data, int size);
    BOOL                encode(AVFrame * pFrame);
    void                addCallback(VideoDataCallback pCallback, void *pUserdata);
    void                delCallback(VideoDataCallback pCallback, void *pUserdata);
    char *              getAuxSDPLine(int rtp_pt);
    
private:    
    void                flush();    
    BOOL                isCallbackExist(VideoDataCallback pCallback, void *pUserdata);
    void                procData(uint8 * data, int size);

    char *              getH264AuxSDPLine(int rtp_pt);
    char *              getH265AuxSDPLine(int rtp_pt);
    char *              getMP4AuxSDPLine(int rtp_pt);
    int                 computeBitrate(AVCodecID codec, int width, int height, int framerate, int quality);

    
private:
    AVCodecID           m_nCodecId;    
    AVPixelFormat       m_nDstPixFmt;

    VideoEncoderParam   m_EncoderParams;

    AVCodecContext *    m_pCodecCtx;
    AVFrame *           m_pFrameSrc;
    AVFrame *           m_pFrameYuv;
    SwsContext *        m_pSwsCtx;    
    uint8 *	            m_pYuvData;

    int                 m_nFrameIndex;
    BOOL                m_bInited;

    void *              m_pCallbackMutex;
	LINKED_LIST *       m_pCallbackList;
};

#endif // _VIDEO_ENCODER_H



