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

#include "sys_inc.h"
#include "audio_decoder.h"


CAudioDecoder::CAudioDecoder()
{
    m_bInited = FALSE;
    m_nCodec = AUDIO_CODEC_NONE;
	m_nSamplerate = 0;
	m_nChannels = 0;

	// m_pCodec = NULL;
	m_pContext = NULL;
	m_pFrame = NULL;
	m_pSwrCtx = NULL;

	m_pCallback = NULL;
	m_pUserdata = NULL;
}

CAudioDecoder::~CAudioDecoder()
{
	uninit();
}

BOOL CAudioDecoder::init(int codec, int samplerate, int channels, uint8 * extradata, int extradatasize)
{
	if (AUDIO_CODEC_G711A == codec)
	{
		m_pCodec = avcodec_find_decoder(AV_CODEC_ID_PCM_ALAW);
	}
	else if (AUDIO_CODEC_G711U == codec)
	{
		m_pCodec = avcodec_find_decoder(AV_CODEC_ID_PCM_MULAW);
	}
	else if (AUDIO_CODEC_G726 == codec)
	{
		m_pCodec = avcodec_find_decoder(AV_CODEC_ID_ADPCM_G726);
	}
	else if (AUDIO_CODEC_AAC == codec)
	{
		m_pCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	}
	else if (AUDIO_CODEC_G722 == codec)
	{
	    m_pCodec = avcodec_find_decoder(AV_CODEC_ID_ADPCM_G722);
	}
	else if (AUDIO_CODEC_OPUS == codec)
	{
	    m_pCodec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	}

	if (m_pCodec == NULL)
	{
	    log_print(LOG_ERR, "%s, m_pCodec is NULL\r\n", __FUNCTION__);
		return FALSE;
	}

	m_pContext = avcodec_alloc_context3(m_pCodec);
	if (NULL == m_pContext)
	{
	    log_print(LOG_ERR, "%s, avcodec_alloc_context3 failed\r\n", __FUNCTION__);
		return FALSE;
	}

	m_pContext->sample_rate = samplerate;
	m_pContext->channels = channels;
	m_pContext->sample_fmt = AV_SAMPLE_FMT_S16;
	m_pContext->channel_layout = av_get_default_channel_layout(channels);

	if (AUDIO_CODEC_G726 == codec)
	{
		m_pContext->bits_per_coded_sample = 32/8;
    	m_pContext->bit_rate = m_pContext->bits_per_coded_sample * m_pContext->sample_rate;
    }

    if (extradatasize > 0 && extradata)
    {
    	m_pContext->extradata = (uint8 *)av_malloc(extradatasize + AV_INPUT_BUFFER_PADDING_SIZE);
	    if (m_pContext->extradata)
	    {
	        memset(m_pContext->extradata + extradatasize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	        memcpy(m_pContext->extradata, extradata, extradatasize);
	        m_pContext->extradata_size = extradatasize;
	    }
    }

	if (avcodec_open2(m_pContext, m_pCodec, NULL) < 0)
	{
	    log_print(LOG_ERR, "%s, avcodec_open2 failed\r\n", __FUNCTION__);
		return FALSE;
	}

	m_pFrame = av_frame_alloc();
	if (NULL == m_pFrame)
	{
	    log_print(LOG_ERR, "%s, av_frame_alloc failed\r\n", __FUNCTION__);
		return FALSE;
	}

	m_nCodec = codec;
	m_nSamplerate = samplerate;
	m_nChannels = channels;

	m_bInited = TRUE;

	return TRUE;
}

BOOL CAudioDecoder::decode(uint8 * data, int len)
{
	AVPacket packet;

	av_init_packet(&packet);

	packet.data = data;
	packet.size = len;

	return decode(&packet);
}

BOOL CAudioDecoder::decode(AVPacket *pkt)
{
    int ret;

    if (!m_bInited)
	{
		return FALSE;
	}

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(m_pContext, pkt);
    if (ret < 0)
    {
        log_print(LOG_ERR, "%s, error submitting the packet to the decoder\r\n", __FUNCTION__);
        return FALSE;
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0)
    {
        ret = avcodec_receive_frame(m_pContext, m_pFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return FALSE;
        }
        else if (ret < 0)
        {
            log_print(LOG_ERR, "%s, error during decoding\r\n", __FUNCTION__);
            return FALSE;
        }

        render();
    }

    return TRUE;
}

void CAudioDecoder::flush()
{
	if (NULL == m_pContext ||
		NULL == m_pContext->codec ||
		!(m_pContext->codec->capabilities | AV_CODEC_CAP_DELAY))
	{
		return;
	}

	decode(NULL);
}

void CAudioDecoder::uninit()
{
	flush();

	if (m_pContext)
	{
		avcodec_close(m_pContext);
		avcodec_free_context(&m_pContext);
	}

	if (m_pFrame)
	{
	    av_frame_free(&m_pFrame);
	}

	if (m_pSwrCtx)
	{
		swr_free(&m_pSwrCtx);
	}

	m_bInited = FALSE;
}

void CAudioDecoder::render()
{
    if (NULL == m_pFrame)
    {
        return;
    }

	if (m_pFrame->sample_rate != m_nSamplerate ||
		m_pFrame->channels != m_nChannels ||
		m_pFrame->format != AV_SAMPLE_FMT_S16)
	{
		if (m_pSwrCtx)
		{
			swr_free(&m_pSwrCtx);
		}

		m_pSwrCtx = swr_alloc_set_opts(NULL,
			av_get_default_channel_layout(m_nChannels), AV_SAMPLE_FMT_S16, m_nSamplerate,
			av_get_default_channel_layout(m_pFrame->channels), (enum AVSampleFormat)m_pFrame->format, m_pFrame->sample_rate, 0, NULL);

		swr_init(m_pSwrCtx);
	}

	if (m_pSwrCtx)
	{
		AVFrame * pResampleFrame = av_frame_alloc();

		pResampleFrame->sample_rate = m_nSamplerate;
		pResampleFrame->format = AV_SAMPLE_FMT_S16;
		pResampleFrame->channels = m_nChannels;
		pResampleFrame->channel_layout = av_get_default_channel_layout(m_nChannels);

		int swrret = swr_convert_frame(m_pSwrCtx, pResampleFrame, m_pFrame);
		if (swrret == 0)
		{
			if (m_pCallback)
			{
				m_pCallback(pResampleFrame, m_pUserdata);
			}
		}

		av_frame_free(&pResampleFrame);
	}
	else if (m_pCallback)
	{
		m_pCallback(m_pFrame, m_pUserdata);
	}
}



