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

#ifndef _AUDIO_ENCODER_H
#define _AUDIO_ENCODER_H

#include "linked_list.h"

extern "C" 
{
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

typedef void (*AudioDataCallback)(uint8 *data, int size, int nbsamples, void *pUserdata);

typedef struct
{
	AudioDataCallback pCallback;    // callback function pointer
	void *            pUserdata;    // user data
} AudioEncoderCB;

/**
 * Some encoders require a fixed frame size
 */
typedef struct
{
    int             tlen;           // buffer total length
    uint8 *         data[2];        // audio data buffer
    int             size[2];        // audio data size
    int             samples;        // audio sample numbers
} AudioBuffer;

typedef struct
{    
    int             SrcSamplerate; // source sample rate
    int             SrcChannels;   // source channels 
    AVSampleFormat  SrcSamplefmt;  // source sample format, see AVSampleFormat

    int             DstCodec;      // output codec
    int             DstSamplerate; // output sample rate
    int             DstChannels;   // output channels
    int             DstBitrate;    // output bitrate
    AVSampleFormat  DstSamplefmt;  // output sample format, see AVSampleFormat
} AudioEncoderParam;

class CAudioEncoder
{
public:
    CAudioEncoder();
    ~CAudioEncoder();

    static AVCodecID    toAVCodecID(int codec);
    
    BOOL                init(AudioEncoderParam * params);
    void                uninit();
    BOOL                encode(uint8 * data, int size);
    BOOL                encode(AVFrame * pFrame);
    void                addCallback(AudioDataCallback pCallback, void *pUserdata);
    void                delCallback(AudioDataCallback pCallback, void *pUserdata);
    char *              getAuxSDPLine(int rtp_pt);
    
private:  
    void                flush();
    BOOL                encodeInternal(AVFrame * pFrame);
    void                procData(uint8 * data, int size, int nbsamples);
    BOOL                isCallbackExist(AudioDataCallback pCallback, void *pUserdata);
    char *              getAACAuxSDPLine(int rtp_pt);
    BOOL                bufferFrame(AVFrame * pFrame);
    int                 computeBitrate(AVCodecID codec, int samplerate, int channels, int quality);
    
private:
    AVCodecID           m_nCodecId;
    BOOL                m_bInited;

    AudioEncoderParam   m_EncoderParams;
    
    AVCodecContext *    m_pCodecCtx;
    AVFrame *           m_pFrame;
    SwrContext *        m_pSwrCtx;
    AudioBuffer         m_AudioBuffer;
    AVPacket *          m_pPkt;
    
    void *              m_pCallbackMutex;
	LINKED_LIST *       m_pCallbackList;
};

#endif // _AUDIO_ENCODER_H



