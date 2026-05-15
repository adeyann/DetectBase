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

#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include "sys_inc.h"
#include "media_format.h"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
#include "libavformat/avformat.h"
#include <libswresample/swresample.h>
}

typedef void (*ADCB)(AVFrame * frame, void * userdata);

class CAudioDecoder
{
public:
    CAudioDecoder();
    ~CAudioDecoder();

    BOOL        init(int codec, int samplerate, int channels, uint8 * extradata = NULL, int extradatasize = 0);
    void        uninit();

    BOOL        decode(uint8 * data, int len);
    BOOL        decode(AVPacket *pkt);
    void        setCallback(ADCB pCallback, void * pUserdata) {m_pCallback = pCallback; m_pUserdata = pUserdata;}

private:
    void        flush();
    void        render();

private:
    BOOL			m_bInited;
    int		        m_nCodec;
    int             m_nSamplerate;
    int             m_nChannels;

    const AVCodec*  m_pCodec = nullptr;
    AVCodecContext* m_pContext;
    AVFrame*        m_pFrame;
    SwrContext*     m_pSwrCtx;

    ADCB            m_pCallback;
    void*           m_pUserdata;
};

#endif


