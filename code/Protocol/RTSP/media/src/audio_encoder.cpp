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
#include "audio_encoder.h"
#include "media_format.h"


CAudioEncoder::CAudioEncoder()
{
	m_nCodecId = AV_CODEC_ID_NONE;
    m_bInited = FALSE;

    memset(&m_EncoderParams, 0, sizeof(AudioEncoderParam));

    m_pCodecCtx = NULL;
    m_pFrame = NULL;
    m_pSwrCtx = NULL;
    m_pPkt = NULL;

    memset(&m_AudioBuffer, 0, sizeof(AudioBuffer));

	m_pCallbackMutex = sys_os_create_mutex();
    m_pCallbackList = h_list_create(FALSE);
}

CAudioEncoder::~CAudioEncoder()
{
	uninit();

	h_list_free_container(m_pCallbackList);

	sys_os_destroy_sig_mutex(m_pCallbackMutex);
}

AVCodecID CAudioEncoder::toAVCodecID(int codec)
{
	AVCodecID codecid = AV_CODEC_ID_NONE;

	switch (codec)
	{
	case AUDIO_CODEC_G711A:
		codecid = AV_CODEC_ID_PCM_ALAW;
		break;

	case AUDIO_CODEC_G711U:
		codecid = AV_CODEC_ID_PCM_MULAW;
		break;

	case AUDIO_CODEC_G726:
		codecid = AV_CODEC_ID_ADPCM_G726;
		break;

	case AUDIO_CODEC_G722:
		codecid = AV_CODEC_ID_ADPCM_G722;
		break;

    case AUDIO_CODEC_OPUS:
        codecid = AV_CODEC_ID_OPUS;
        break;

	case AUDIO_CODEC_AAC:
		codecid = AV_CODEC_ID_AAC;
		break;
	}

	return codecid;
}

int CAudioEncoder::computeBitrate(AVCodecID codec, int samplerate, int channels, int quality)
{
	int bitrate;

	if (m_nCodecId == AV_CODEC_ID_ADPCM_G726)
    {
    	bitrate = 32000; // G726 32kbit/s
    }
    else if (m_nCodecId == AV_CODEC_ID_ADPCM_G722)
    {
    	bitrate = 64000; // G722 64kbit/s
    }
    else if (m_nCodecId == AV_CODEC_ID_PCM_ALAW || m_nCodecId == AV_CODEC_ID_PCM_MULAW)
    {
    	bitrate = samplerate * channels * 8;
    }
	else if (m_nCodecId == AV_CODEC_ID_AAC)
	{
		bitrate = samplerate * channels * 16 / 7;
	}
	else
	{
		bitrate = samplerate * channels;
	}

    return bitrate;
}

BOOL CAudioEncoder::init(AudioEncoderParam * params)
{
	memcpy(&m_EncoderParams, params, sizeof(AudioEncoderParam));

	m_nCodecId = toAVCodecID(params->DstCodec);

	if (AV_CODEC_ID_AAC == m_nCodecId)
	{
		// the ffmepg AAC encoder only support AV_SAMPLE_FMT_FLTP
		m_EncoderParams.DstSamplefmt = AV_SAMPLE_FMT_FLTP;
	}

	const AVCodec * pCodec = avcodec_find_encoder(m_nCodecId);
	if (pCodec == NULL)
	{
		log_print(LOG_ERR, "avcodec_find_encoder failed, %d\r\n", m_nCodecId);
		return FALSE;
	}

	m_pCodecCtx = avcodec_alloc_context3(pCodec);
	if (m_pCodecCtx == NULL)
	{
		log_print(LOG_ERR, "avcodec_alloc_context3 failed\r\n");
		return FALSE;
	}

	m_pCodecCtx->codec_id = m_nCodecId;
	m_pCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
	m_pCodecCtx->qblur = 0.5f;
	m_pCodecCtx->time_base.num = 1;
	m_pCodecCtx->time_base.den = m_EncoderParams.DstSamplerate;
	m_pCodecCtx->sample_rate = m_EncoderParams.DstSamplerate;
    m_pCodecCtx->channels = m_EncoderParams.DstChannels;
    m_pCodecCtx->channel_layout = av_get_default_channel_layout(m_EncoderParams.DstChannels);
    m_pCodecCtx->sample_fmt = m_EncoderParams.DstSamplefmt;

    if (m_EncoderParams.DstBitrate > 0)
	{
	    m_pCodecCtx->bit_rate = m_EncoderParams.DstBitrate * 1000;
	}
	else
	{
	    m_pCodecCtx->bit_rate = computeBitrate(m_nCodecId, m_EncoderParams.DstSamplerate, m_EncoderParams.DstChannels, 80);
	}

	m_pCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	av_opt_set(m_pCodecCtx->priv_data, "preset", "superfast", 0);
	av_opt_set(m_pCodecCtx->priv_data, "tune", "zerolatency", 0);

	if (avcodec_open2(m_pCodecCtx, pCodec, NULL) < 0)
	{
		log_print(LOG_ERR, "avcodec_open2 failed, audio encoder\r\n");
		return FALSE;
	}

	m_pFrame = av_frame_alloc();
	if (NULL == m_pFrame)
	{
		return FALSE;
	}

	if (m_EncoderParams.SrcSamplerate != m_EncoderParams.DstSamplerate ||
		m_EncoderParams.SrcChannels != m_EncoderParams.DstChannels ||
		m_EncoderParams.SrcSamplefmt != m_EncoderParams.DstSamplefmt)
	{
		m_pSwrCtx = swr_alloc_set_opts(NULL,
			av_get_default_channel_layout(m_EncoderParams.DstChannels), m_EncoderParams.DstSamplefmt, m_EncoderParams.DstSamplerate,
			av_get_default_channel_layout(m_EncoderParams.SrcChannels), m_EncoderParams.SrcSamplefmt, m_EncoderParams.SrcSamplerate, 0, NULL);

		swr_init(m_pSwrCtx);
	}

	if (m_pCodecCtx->frame_size == 0)
	{
		m_pCodecCtx->frame_size = 1024;
	}

	m_AudioBuffer.tlen = 4 * m_pCodecCtx->frame_size * m_EncoderParams.DstChannels * av_get_bytes_per_sample(m_EncoderParams.DstSamplefmt);
	m_AudioBuffer.data[0] = (uint8 *)av_malloc(m_AudioBuffer.tlen);
	m_AudioBuffer.data[1] = (uint8 *)av_malloc(m_AudioBuffer.tlen);
	m_AudioBuffer.size[0] = 0;
	m_AudioBuffer.size[1] = 0;
	m_AudioBuffer.samples = 0;

	/* packet for holding encoded output */
	m_pPkt = av_packet_alloc();
	if (!m_pPkt)
	{
		log_print(LOG_ERR, "could not allocate the packet\r\n");
		return FALSE;
	}

	m_bInited = TRUE;

	return TRUE;
}

void CAudioEncoder::uninit()
{
	flush();

	if (m_pCodecCtx)
	{
		avcodec_close(m_pCodecCtx);
		avcodec_free_context(&m_pCodecCtx);
	}

	if (m_pFrame)
	{
		av_frame_free(&m_pFrame);
	}

	if (m_pSwrCtx)
	{
		swr_free(&m_pSwrCtx);
	}

	if (m_pPkt)
	{
		av_packet_free(&m_pPkt);
	}

	if (m_AudioBuffer.data[0])
	{
		av_freep(&m_AudioBuffer.data[0]);
	}

	if (m_AudioBuffer.data[1])
	{
		av_freep(&m_AudioBuffer.data[1]);
	}

	m_bInited = FALSE;
}

BOOL CAudioEncoder::bufferFrame(AVFrame * pFrame)
{
	BOOL ret = TRUE;
	int samplesize = av_get_bytes_per_sample((AVSampleFormat)pFrame->format);

	int size = pFrame->nb_samples * samplesize;

	assert(m_AudioBuffer.size[0] + size <= m_AudioBuffer.tlen);

	if (av_sample_fmt_is_planar((AVSampleFormat)pFrame->format) && m_EncoderParams.DstChannels > 1)
	{
		memcpy(m_AudioBuffer.data[0]+m_AudioBuffer.size[0], pFrame->data[0], size);
		m_AudioBuffer.size[0] += size;

		memcpy(m_AudioBuffer.data[1]+m_AudioBuffer.size[1], pFrame->data[1], size);
		m_AudioBuffer.size[1] += size;
	}
	else
	{
		memcpy(m_AudioBuffer.data[0]+m_AudioBuffer.size[0], pFrame->data[0], size * m_EncoderParams.DstChannels);
		m_AudioBuffer.size[0] += size * m_EncoderParams.DstChannels;
	}

	m_AudioBuffer.samples += pFrame->nb_samples;

	while (m_AudioBuffer.samples >= m_pCodecCtx->frame_size)
	{
		int linesize = samplesize * m_pCodecCtx->frame_size;

		if (av_sample_fmt_is_planar((AVSampleFormat)pFrame->format) && m_EncoderParams.DstChannels > 1)
		{
			linesize = samplesize * m_pCodecCtx->frame_size;
		}
		else
		{
			linesize = samplesize * m_pCodecCtx->frame_size * m_EncoderParams.DstChannels;
		}

		m_pFrame->data[0] = m_AudioBuffer.data[0];
		m_pFrame->data[1] = m_AudioBuffer.data[1];
		m_pFrame->linesize[0] = linesize;
		m_pFrame->linesize[1] = linesize;
		m_pFrame->nb_samples = m_pCodecCtx->frame_size;
		m_pFrame->format = m_EncoderParams.DstSamplefmt;
		m_pFrame->key_frame = 1;
		m_pFrame->sample_rate = m_EncoderParams.DstSamplerate;
		m_pFrame->channels = m_EncoderParams.DstChannels;
		m_pFrame->channel_layout = av_get_default_channel_layout(m_EncoderParams.DstChannels);

		ret = encodeInternal(m_pFrame);

		memmove(m_AudioBuffer.data[0], m_AudioBuffer.data[0]+linesize, m_AudioBuffer.size[0] - linesize);
		m_AudioBuffer.size[0] -= linesize;

		if (av_sample_fmt_is_planar((AVSampleFormat)pFrame->format) && m_EncoderParams.DstChannels > 1)
		{
			memmove(m_AudioBuffer.data[1], m_AudioBuffer.data[1]+linesize, m_AudioBuffer.size[1] - linesize);
			m_AudioBuffer.size[1] -= linesize;
		}

		m_AudioBuffer.samples -= m_pCodecCtx->frame_size;
	}

	return ret;
}

void CAudioEncoder::flush()
{
	if (NULL == m_pCodecCtx ||
		NULL == m_pCodecCtx->codec ||
		!(m_pCodecCtx->codec->capabilities | AV_CODEC_CAP_DELAY))
	{
		return;
	}

	encodeInternal(NULL);
}

BOOL CAudioEncoder::encodeInternal(AVFrame * pFrame)
{
	int ret;

	/* send the frame for encoding */
	ret = avcodec_send_frame(m_pCodecCtx, pFrame);
	if (ret < 0)
	{
		log_print(LOG_ERR, "error sending the frame to the encoder\r\n");
		return FALSE;
	}

	/* read all the available output packets (in general there may be any
	 * number of them */
	while (ret >= 0)
	{
		ret = avcodec_receive_packet(m_pCodecCtx, m_pPkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			return TRUE;
		}
		else if (ret < 0)
		{
			log_print(LOG_ERR, "error encoding audio frame\r\n");
			return FALSE;
		}

		procData(m_pPkt->data, m_pPkt->size, pFrame ? pFrame->nb_samples : 0);

		av_packet_unref(m_pPkt);
	}

	return TRUE;
}

BOOL CAudioEncoder::encode(uint8 * data, int size)
{
	if (!m_bInited)
	{
		return FALSE;
	}

	m_pFrame->data[0] = data;
	m_pFrame->linesize[0] = size;
	m_pFrame->nb_samples = size / (m_EncoderParams.SrcChannels * av_get_bytes_per_sample(m_EncoderParams.SrcSamplefmt));
	m_pFrame->format = m_EncoderParams.SrcSamplefmt;
	m_pFrame->key_frame = 1;
	m_pFrame->sample_rate = m_EncoderParams.SrcSamplerate;
	m_pFrame->channels = m_EncoderParams.SrcChannels;
	m_pFrame->channel_layout = av_get_default_channel_layout(m_EncoderParams.SrcChannels);

	return encode(m_pFrame);
}

BOOL CAudioEncoder::encode(AVFrame * pFrame)
{
	BOOL ret = TRUE;

	if (!m_bInited)
	{
		return FALSE;
	}

	if (m_pSwrCtx)
	{
		AVFrame * pResampleFrame = av_frame_alloc();

		pResampleFrame->sample_rate = m_EncoderParams.DstSamplerate;
		pResampleFrame->format = m_EncoderParams.DstSamplefmt;
		pResampleFrame->channels = m_EncoderParams.DstChannels;
		pResampleFrame->channel_layout = av_get_default_channel_layout(m_EncoderParams.DstChannels);

		int swrret = swr_convert_frame(m_pSwrCtx, pResampleFrame, pFrame);
		if (swrret == 0)
		{
			ret = bufferFrame(pResampleFrame);
		}
		else
		{
			ret = FALSE;
		}

		av_frame_free(&pResampleFrame);
	}
	else
	{
		ret = bufferFrame(pFrame);
	}

	return ret;
}

void CAudioEncoder::procData(uint8 * data, int size, int nbsamples)
{
	AudioEncoderCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (AudioEncoderCB *) p_node->p_data;
		if (p_cb->pCallback != NULL)
		{
			p_cb->pCallback(data, size, nbsamples, p_cb->pUserdata);
		}

		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

BOOL CAudioEncoder::isCallbackExist(AudioDataCallback pCallback, void *pUserdata)
{
	BOOL exist = FALSE;
	AudioEncoderCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (AudioEncoderCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata)
		{
			exist = TRUE;
			break;
		}

		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);

	return exist;
}

void CAudioEncoder::addCallback(AudioDataCallback pCallback, void *pUserdata)
{
	if (isCallbackExist(pCallback, pUserdata))
	{
		return;
	}

	AudioEncoderCB * p_cb = (AudioEncoderCB *) malloc(sizeof(AudioEncoderCB));

	p_cb->pCallback = pCallback;
	p_cb->pUserdata = pUserdata;

	sys_os_mutex_enter(m_pCallbackMutex);
	h_list_add_at_back(m_pCallbackList, p_cb);
	sys_os_mutex_leave(m_pCallbackMutex);
}

void CAudioEncoder::delCallback(AudioDataCallback pCallback, void *pUserdata)
{
	AudioEncoderCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (AudioEncoderCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata)
		{
			free(p_cb);

			h_list_remove(m_pCallbackList, p_node);
			break;
		}

		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

char * CAudioEncoder::getAACAuxSDPLine(int rtp_pt)
{
	if (NULL == m_pCodecCtx || m_pCodecCtx->extradata_size == 0)
	{
		return NULL;
	}

	char const* fmtpFmt =
        "a=fmtp:%d "
    	"streamtype=5;profile-level-id=1;"
    	"mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;"
    	"config=";
	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    + 3 /* max char len */
	    + 2*m_pCodecCtx->extradata_size; /* 2*, because each byte prints as 2 chars */

	char* fmtp = new char[fmtpFmtSize+1];
	memset(fmtp, 0, fmtpFmtSize+1);

	sprintf(fmtp, fmtpFmt, rtp_pt);
	char* endPtr = &fmtp[strlen(fmtp)];
	for (int i = 0; i < m_pCodecCtx->extradata_size; ++i)
	{
		sprintf(endPtr, "%02X", m_pCodecCtx->extradata[i]);
		endPtr += 2;
	}

	return fmtp;
}

char * CAudioEncoder::getAuxSDPLine(int rtp_pt)
{
	if (m_nCodecId == AV_CODEC_ID_AAC)
	{
		return getAACAuxSDPLine(rtp_pt);
	}

	return NULL;
}




