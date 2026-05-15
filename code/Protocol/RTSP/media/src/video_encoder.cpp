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
#include "video_encoder.h"
#include "media_format.h"
#include "base64.h"
#include "media_util.h"
#include "h264.h"
#include "h265.h"
#include "h264_util.h"

CVideoEncoder::CVideoEncoder()
{
    m_nCodecId		 = AV_CODEC_ID_NONE;
    m_nDstPixFmt	 = AV_PIX_FMT_YUV420P;

    m_pCodecCtx		 = NULL;
    m_pFrameSrc		 = NULL;
    m_pFrameYuv		 = NULL;
    m_pSwsCtx		 = NULL;
    m_pYuvData		 = NULL;

    m_nFrameIndex	 = 0;
    m_bInited		 = FALSE;

    m_pCallbackMutex = sys_os_create_mutex();
    m_pCallbackList  = h_list_create(FALSE);
}

CVideoEncoder::~CVideoEncoder()
{
	uninit();

	h_list_free_container(m_pCallbackList);

	sys_os_destroy_sig_mutex(m_pCallbackMutex);
}

AVCodecID CVideoEncoder::toAVCodecID(int codec)
{
	AVCodecID codecid = AV_CODEC_ID_NONE;

	switch (codec)
	{
	case VIDEO_CODEC_H264:
		codecid = AV_CODEC_ID_H264;
		break;

	case VIDEO_CODEC_H265:
		codecid = AV_CODEC_ID_HEVC;
		break;

	case VIDEO_CODEC_MP4:
		codecid = AV_CODEC_ID_MPEG4;
		break;

	case VIDEO_CODEC_JPEG:
		codecid = AV_CODEC_ID_MJPEG;
		break;
	}

	return codecid;
}

AVPixelFormat CVideoEncoder::toAVPixelFormat(int pixfmt)
{
	AVPixelFormat avpixfmt = AV_PIX_FMT_NONE;

	switch (pixfmt)
	{
	case VIDEO_FMT_YUYV422:
		avpixfmt = AV_PIX_FMT_YUYV422;
		break;

	case VIDEO_FMT_YUV420P:
		avpixfmt = AV_PIX_FMT_YUV420P;
		break;

	case VIDEO_FMT_YVYU422:
		avpixfmt = AV_PIX_FMT_YVYU422;
		break;

	case VIDEO_FMT_UYVY422:
		avpixfmt = AV_PIX_FMT_UYVY422;
		break;

	case VIDEO_FMT_NV12:
		avpixfmt = AV_PIX_FMT_NV12;
		break;

    case VIDEO_FMT_NV21:
		avpixfmt = AV_PIX_FMT_NV21;
		break;

	case VIDEO_FMT_RGB24:
		avpixfmt = AV_PIX_FMT_RGB24;
		break;

	case VIDEO_FMT_RGB32:
		avpixfmt = AV_PIX_FMT_RGB32;
		break;

	case VIDEO_FMT_ARGB:
		avpixfmt = AV_PIX_FMT_ARGB;
		break;

	case VIDEO_FMT_BGRA:
		avpixfmt = AV_PIX_FMT_BGRA;
		break;
	}

	return avpixfmt;
}

int CVideoEncoder::computeBitrate(AVCodecID codec, int width, int height, int framerate, int quality)
{
    double fLQ = 0;
    double fMQ = 0;
    double fHQ = 0;

    int factor = 2;
    int pixels = width * height;

    if (quality>= 0 && quality < 33)
    {
        factor = 0;
    }
    else if (quality >= 33 && quality < 67)
    {
        factor = 1;
    }
    else if (quality >= 67 && quality <= 100)
    {
        factor = 2;
    }

    switch (codec)
    {
    case AV_CODEC_ID_H264:
        if (pixels < 640*480)
        {
            fLQ = 12/70.0;
            fMQ = 12/55.0;
            fHQ = 12/40.0;
        }
        else
        {
            fLQ = 12/90.0;
            fMQ = 12/70.0;
            fHQ = 12/60.0;
        }
        break;

    case AV_CODEC_ID_MJPEG:
        if (pixels < 640*480)
        {
            fLQ = 12/40.0;
            fMQ = 12/30.0;
            fHQ = 12/20.0;
        }
        else
        {
            fLQ = 12/50.0;
            fMQ = 12/40.0;
            fHQ = 12/30.0;
        }
        break;

    default:
        if (pixels < 640*480)
        {
            fLQ = 12/55.0;
            fMQ = 12/40.0;
            fHQ = 12/30.0;
        }
        else
        {
            fLQ = 12/75.0;
            fMQ = 12/60.0;
            fHQ = 12/50.0;
        }
        break;
    }

    int nBitrate = 0;

    if (0 == factor)
    {
        nBitrate = (int) (pixels * framerate * fLQ);
    }
    else if (1 == factor)
    {
        nBitrate =  (int) (pixels * framerate * fMQ);
    }
    else
    {
        nBitrate = (int) (pixels * framerate * fHQ);
    }

    if (quality < 40)
    {
        quality = 40;
    }
    else if (quality > 80)
    {
        quality = 80;
    }

    nBitrate = nBitrate * (quality / 100.0);

    return nBitrate;
}

BOOL CVideoEncoder::init(VideoEncoderParam * params)
{
	memcpy(&m_EncoderParams, params, sizeof(VideoEncoderParam));

	if (m_EncoderParams.DstHeight < 0)
		m_EncoderParams.DstHeight = -m_EncoderParams.DstHeight;

	m_nCodecId = toAVCodecID(m_EncoderParams.DstCodec);

	const AVCodec * pCodec = avcodec_find_encoder(m_nCodecId);
	if (pCodec == NULL) {
		log_print(LOG_ERR, "avcodec_find_encoder failed, %d\r\n", m_nCodecId);
		return FALSE;
	}

	m_pCodecCtx = avcodec_alloc_context3(pCodec);
	if (m_pCodecCtx == NULL) {
		log_print(LOG_ERR, "avcodec_alloc_context3 failed\r\n");
		return FALSE;
	}

	// set destination pixmel format
	if (AV_CODEC_ID_MJPEG == m_nCodecId)
		m_nDstPixFmt = AV_PIX_FMT_YUVJ420P;
	else
		m_nDstPixFmt = AV_PIX_FMT_YUV420P;

	// set video encoder parameters
	m_pCodecCtx->codec_id		= m_nCodecId;
	m_pCodecCtx->codec_type		= AVMEDIA_TYPE_VIDEO;
	m_pCodecCtx->width			= m_EncoderParams.DstWidth;
	m_pCodecCtx->height			= m_EncoderParams.DstHeight;
	m_pCodecCtx->gop_size		= 60;
	m_pCodecCtx->pix_fmt		= m_nDstPixFmt;
	m_pCodecCtx->qblur			= 0.5f;
	m_pCodecCtx->time_base.num	= 1;
	m_pCodecCtx->time_base.den	= m_EncoderParams.DstFramerate;
	m_pCodecCtx->framerate.num	= m_EncoderParams.DstFramerate;
	m_pCodecCtx->framerate.den	= 1;
	m_pCodecCtx->qmin			= 3;
	m_pCodecCtx->qmax			= 31;
	m_pCodecCtx->delay			= 0;
	m_pCodecCtx->max_b_frames	= 0;

	if (m_EncoderParams.DstBitrate > 0) {
	    m_pCodecCtx->bit_rate = m_EncoderParams.DstBitrate * 1000;
	}
	else {
	    m_pCodecCtx->bit_rate = computeBitrate(m_nCodecId, m_EncoderParams.DstWidth, m_EncoderParams.DstHeight, m_EncoderParams.DstFramerate, 100);
	}

	if (m_nCodecId == AV_CODEC_ID_MPEG4) {
		m_pCodecCtx->profile = FF_PROFILE_MPEG4_SIMPLE;
		m_pCodecCtx->codec_tag = 0x30355844;
	}
	else if (m_nCodecId == AV_CODEC_ID_H264) {
		m_pCodecCtx->profile = FF_PROFILE_H264_MAIN;
	}
	else if (m_nCodecId == AV_CODEC_ID_MJPEG) {
		m_pCodecCtx->color_range = AVCOL_RANGE_JPEG;
		av_opt_set(m_pCodecCtx->priv_data, "huffman", "default", 0);
	}

	m_pCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	av_opt_set(m_pCodecCtx->priv_data, "preset", "superfast", 0);
	av_opt_set(m_pCodecCtx->priv_data, "tune", "zerolatency", 0);

	if (avcodec_open2(m_pCodecCtx, pCodec, NULL) < 0) {
		log_print(LOG_ERR, "avcodec_open2 failed, video encoder\r\n");
		return FALSE;
	}

	if (m_EncoderParams.SrcPixFmt != m_nDstPixFmt ||
		m_EncoderParams.SrcWidth != m_EncoderParams.DstWidth ||
		m_EncoderParams.SrcHeight != m_EncoderParams.DstHeight) {

		m_pFrameSrc = av_frame_alloc();
		if (NULL == m_pFrameSrc)
			return FALSE;

		m_pFrameSrc->format = m_EncoderParams.SrcPixFmt;
		m_pFrameSrc->width  = m_EncoderParams.SrcWidth;
		m_pFrameSrc->height = m_EncoderParams.SrcHeight;

		m_pFrameYuv = av_frame_alloc();
		if (NULL == m_pFrameYuv)
			return FALSE;

		m_pFrameYuv->format = m_nDstPixFmt;
		m_pFrameYuv->width  = m_EncoderParams.DstWidth;
		m_pFrameYuv->height = m_EncoderParams.DstHeight;

		m_pSwsCtx = sws_getContext(m_EncoderParams.SrcWidth, m_EncoderParams.SrcHeight, m_EncoderParams.SrcPixFmt,
								   m_EncoderParams.DstWidth, m_EncoderParams.DstHeight, m_nDstPixFmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);

        int yuv_size = av_image_get_buffer_size(m_nDstPixFmt, m_EncoderParams.DstWidth, m_EncoderParams.DstHeight, 32);

		m_pYuvData = (uint8 *)av_malloc(yuv_size);
		if (m_pYuvData) {
		    av_image_fill_arrays(m_pFrameYuv->data, m_pFrameYuv->linesize, m_pYuvData, m_nDstPixFmt, m_EncoderParams.DstWidth, m_EncoderParams.DstHeight, 32);
		}
		else {
		    log_print(LOG_ERR, "%s, av_malloc failed\r\n", __FUNCTION__);
			return FALSE;
		}
	}
	else {
		m_pFrameSrc = av_frame_alloc();
		if (NULL == m_pFrameSrc)
			return FALSE;

		m_pFrameSrc->format = m_EncoderParams.SrcPixFmt;
		m_pFrameSrc->width  = m_EncoderParams.SrcWidth;
		m_pFrameSrc->height = m_EncoderParams.SrcHeight;
	}

	m_bInited = TRUE;
	return TRUE;
}

void CVideoEncoder::uninit()
{
	flush();

	if (m_pCodecCtx) {
		avcodec_close(m_pCodecCtx);
		avcodec_free_context(&m_pCodecCtx);
	}

	if (m_pFrameSrc)
		av_frame_free(&m_pFrameSrc);

	if (m_pFrameYuv)
		av_frame_free(&m_pFrameYuv);

	if (m_pSwsCtx) {
		sws_freeContext(m_pSwsCtx);
		m_pSwsCtx = NULL;
	}

	if (m_pYuvData)
		av_freep(&m_pYuvData);

	m_bInited = FALSE;
}

void CVideoEncoder::flush()
{
	if (NULL == m_pCodecCtx ||
		NULL == m_pCodecCtx->codec ||
		!(m_pCodecCtx->codec->capabilities | AV_CODEC_CAP_DELAY)) {
		return;
	}

	encode(NULL);
}

BOOL CVideoEncoder::encode(uint8 * data, int size)
{
	if (!m_bInited)
		return FALSE;

    av_image_fill_arrays(m_pFrameSrc->data, m_pFrameSrc->linesize, data, m_EncoderParams.SrcPixFmt, m_EncoderParams.SrcWidth, m_EncoderParams.SrcHeight, 1);

	return encode(m_pFrameSrc);
}

BOOL CVideoEncoder::encode(AVFrame * pFrame)
{
	if (!m_bInited)
		return FALSE;

	AVFrame * pFrameSrc = NULL;

	if (NULL == m_pSwsCtx) {
		pFrameSrc = pFrame;
	}
	else if (pFrame) {
	    av_frame_make_writable(m_pFrameYuv); // make sure the frame data is writable

		sws_scale(m_pSwsCtx, pFrame->data, pFrame->linesize, 0, pFrame->height, m_pFrameYuv->data, m_pFrameYuv->linesize);
		pFrameSrc = m_pFrameYuv;
	}

    if (pFrameSrc) {
    	if ((m_nFrameIndex % (int)m_EncoderParams.DstFramerate) == 0) {
    		pFrameSrc->pict_type = AV_PICTURE_TYPE_I;
    		pFrameSrc->key_frame = 1;
    	}
    	else {
    		pFrameSrc->pict_type = AV_PICTURE_TYPE_P;
    		pFrameSrc->key_frame = 0;
    	}

    	m_nFrameIndex++;

    	if (m_EncoderParams.Updown) {
    		pFrameSrc->data[0]		+= pFrameSrc->linesize[0] * (m_EncoderParams.DstHeight - 1);
    		pFrameSrc->linesize[0]	*= -1;
    		pFrameSrc->data[1]		+= pFrameSrc->linesize[1] * (m_EncoderParams.DstHeight / 2 - 1);
    		pFrameSrc->linesize[1]	*= -1;
    		pFrameSrc->data[2]		+= pFrameSrc->linesize[2] * (m_EncoderParams.DstHeight / 2 - 1);
    		pFrameSrc->linesize[2]	*= -1;
    	}
	}

    int ret;
    AVPacket pkt;

	av_init_packet(&pkt);
	pkt.data = 0;
	pkt.size = 0;

    ret = avcodec_send_frame(m_pCodecCtx, pFrameSrc);
    if (ret < 0) {
        log_print(LOG_ERR, "%s, error sending a frame for encoding\r\n", __FUNCTION__);
        return FALSE;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(m_pCodecCtx, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return FALSE;
        }
        else if (ret < 0) {
            log_print(LOG_ERR, "%s, error during encoding\r\n", __FUNCTION__);
            return FALSE;
        }

        procData(pkt.data, pkt.size);
        av_packet_unref(&pkt);
    }
	return TRUE;
}

void CVideoEncoder::procData(uint8 * data, int size)
{
	VideoEncoderCB* p_cb   = NULL;
	LINKED_NODE*    p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node) {
		p_cb = (VideoEncoderCB *) p_node->p_data;
		if (p_cb->pCallback != NULL) {
		    if (p_cb->bFirst && (m_nCodecId == AV_CODEC_ID_HEVC || m_nCodecId == AV_CODEC_ID_H264)) {
		        if (m_pCodecCtx && m_pCodecCtx->extradata_size > 8) {
					//rtsp_proxy_vencoder_cb
		            p_cb->pCallback(m_pCodecCtx->extradata, m_pCodecCtx->extradata_size, p_cb->pUserdata);
		        }
		        p_cb->bFirst = FALSE;
		    }
		    //rtsp_proxy_vencoder_cb
			p_cb->pCallback(data, size, p_cb->pUserdata);
		}
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

BOOL CVideoEncoder::isCallbackExist(VideoDataCallback pCallback, void *pUserdata)
{
	BOOL exist = FALSE;
	VideoEncoderCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node) {
		p_cb = (VideoEncoderCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata) {
			exist = TRUE;
			break;
		}
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);

	return exist;
}

void CVideoEncoder::addCallback(VideoDataCallback pCallback, void *pUserdata)
{
	if (isCallbackExist(pCallback, pUserdata))
		return;

	VideoEncoderCB * p_cb = (VideoEncoderCB *) malloc(sizeof(VideoEncoderCB));

	p_cb->pCallback = pCallback;
	p_cb->pUserdata = pUserdata;
	p_cb->bFirst = TRUE;

	sys_os_mutex_enter(m_pCallbackMutex);
	h_list_add_at_back(m_pCallbackList, p_cb);
	sys_os_mutex_leave(m_pCallbackMutex);
}

void CVideoEncoder::delCallback(VideoDataCallback pCallback, void *pUserdata)
{
	VideoEncoderCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;

	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node) {
		p_cb = (VideoEncoderCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata) {
			free(p_cb);

			h_list_remove(m_pCallbackList, p_node);
			break;
		}
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

char * CVideoEncoder::getH264AuxSDPLine(int rtp_pt)
{
	uint8 * sps = NULL; uint32 spsSize = 0;
	uint8 * pps = NULL; uint32 ppsSize = 0;

	if (NULL == m_pCodecCtx || m_pCodecCtx->extradata_size <= 8)
	{
		return NULL;
	}

	uint8 *r, *end = m_pCodecCtx->extradata + m_pCodecCtx->extradata_size;
	r = avc_find_startcode(m_pCodecCtx->extradata, end);

	while (r < end)
	{
        uint8 *r1;

        while (!*(r++));
        r1 = avc_find_startcode(r, end);

        int nal_type = (r[0] & 0x1F);

        if (H264_NAL_PPS == nal_type)
        {
            pps = r;
            ppsSize = r1 - r;
        }
        else if (H264_NAL_SPS == nal_type)
        {
            sps = r;
            spsSize = r1 - r;
        }

        r = r1;
    }

	if (NULL == sps || spsSize == 0 ||
		NULL == pps || ppsSize == 0)
	{
		return NULL;
	}

	// Set up the "a=fmtp:" SDP line for this stream:
	uint8* spsWEB = new uint8[spsSize]; // "WEB" means "Without Emulation Bytes"
  	uint32 spsWEBSize = remove_emulation_bytes(spsWEB, spsSize, sps, spsSize);
  	if (spsWEBSize < 4)
  	{
  		// Bad SPS size => assume our source isn't ready
    	delete[] spsWEB;
    	return NULL;
  	}
  	uint8 profileLevelId = (spsWEB[1]<<16) | (spsWEB[2]<<8) | spsWEB[3];
  	delete[] spsWEB;

  	char* sps_base64 = new char[spsSize*2+1];
  	char* pps_base64 = new char[ppsSize*2+1];

	base64_encode(sps, spsSize, sps_base64, spsSize*2+1);
	base64_encode(pps, ppsSize, pps_base64, ppsSize*2+1);

  	char const* fmtpFmt =
    	"a=fmtp:%d packetization-mode=1"
    	";profile-level-id=%06X"
    	";sprop-parameter-sets=%s,%s";

  	uint32 fmtpFmtSize = strlen(fmtpFmt)
    	+ 3 /* max char len */
    	+ 6 /* 3 bytes in hex */
    	+ strlen(sps_base64) + strlen(pps_base64);

  	char* fmtp = new char[fmtpFmtSize+1];
  	memset(fmtp, 0, fmtpFmtSize+1);

  	sprintf(fmtp, fmtpFmt, rtp_pt, profileLevelId, sps_base64, pps_base64);

  	delete[] sps_base64;
  	delete[] pps_base64;

  	return fmtp;
}

char * CVideoEncoder::getH265AuxSDPLine(int rtp_pt)
{
	uint8* vps = NULL; uint32 vpsSize = 0;
	uint8* sps = NULL; uint32 spsSize = 0;
	uint8* pps = NULL; uint32 ppsSize = 0;

	if (NULL == m_pCodecCtx || m_pCodecCtx->extradata_size < 12)
	{
		return NULL;
	}

    uint8 *r, *end = m_pCodecCtx->extradata + m_pCodecCtx->extradata_size;
	r = avc_find_startcode(m_pCodecCtx->extradata, end);

	while (r < end)
	{
        uint8 *r1;

        while (!*(r++));
        r1 = avc_find_startcode(r, end);

        int nal_type = (r[0] >> 1) & 0x3F;

        if (HEVC_NAL_VPS == nal_type)
        {
            vps = r;
            vpsSize = r1 - r;
        }
        else if (HEVC_NAL_PPS == nal_type)
        {
            pps = r;
            ppsSize = r1 - r;
        }
        else if (HEVC_NAL_SPS == nal_type)
        {
            sps = r;
            spsSize = r1 - r;
        }

        r = r1;
    }

	if (NULL == vps || vpsSize == 0 ||
		NULL == sps || spsSize == 0 ||
		NULL == pps || ppsSize == 0)
	{
		return NULL;
	}

	// Set up the "a=fmtp:" SDP line for this stream.
	uint8* vpsWEB = new uint8[vpsSize]; // "WEB" means "Without Emulation Bytes"
	uint32 vpsWEBSize = remove_emulation_bytes(vpsWEB, vpsSize, vps, vpsSize);
	if (vpsWEBSize < 6/*'profile_tier_level' offset*/ + 12/*num 'profile_tier_level' bytes*/)
	{
		// Bad VPS size => assume our source isn't ready
		delete[] vpsWEB;
		return NULL;
	}

	uint8 const* profileTierLevelHeaderBytes = &vpsWEB[6];
	uint32 profileSpace  = profileTierLevelHeaderBytes[0]>>6; // general_profile_space
	uint32 profileId = profileTierLevelHeaderBytes[0]&0x1F; 	// general_profile_idc
	uint32 tierFlag = (profileTierLevelHeaderBytes[0]>>5)&0x1;// general_tier_flag
	uint32 levelId = profileTierLevelHeaderBytes[11]; 		// general_level_idc
	uint8 const* interop_constraints = &profileTierLevelHeaderBytes[5];
	char interopConstraintsStr[100];
	sprintf(interopConstraintsStr, "%02X%02X%02X%02X%02X%02X",
	interop_constraints[0], interop_constraints[1], interop_constraints[2],
	interop_constraints[3], interop_constraints[4], interop_constraints[5]);
	delete[] vpsWEB;

	char* sprop_vps = new char[vpsSize*2+1];
  	char* sprop_sps = new char[spsSize*2+1];
  	char* sprop_pps = new char[ppsSize*2+1];

	base64_encode(vps, vpsSize, sprop_vps, vpsSize*2+1);
  	base64_encode(sps, spsSize, sprop_sps, spsSize*2+1);
	base64_encode(pps, ppsSize, sprop_pps, ppsSize*2+1);

	char const* fmtpFmt =
		"a=fmtp:%d profile-space=%u"
		";profile-id=%u"
		";tier-flag=%u"
		";level-id=%u"
		";interop-constraints=%s"
		";sprop-vps=%s"
		";sprop-sps=%s"
		";sprop-pps=%s";

	uint32 fmtpFmtSize = strlen(fmtpFmt)
		+ 3 /* max num chars: rtpPayloadType */ + 20 /* max num chars: profile_space */
		+ 20 /* max num chars: profile_id */
		+ 20 /* max num chars: tier_flag */
		+ 20 /* max num chars: level_id */
		+ strlen(interopConstraintsStr)
		+ strlen(sprop_vps)
		+ strlen(sprop_sps)
		+ strlen(sprop_pps);

	char* fmtp = new char[fmtpFmtSize+1];
	memset(fmtp, 0, fmtpFmtSize+1);

	sprintf(fmtp, fmtpFmt,
	  	rtp_pt, profileSpace,
		profileId,
		tierFlag,
		levelId,
		interopConstraintsStr,
		sprop_vps,
		sprop_sps,
		sprop_pps);

	delete[] sprop_vps;
	delete[] sprop_sps;
	delete[] sprop_pps;

	return fmtp;
}

char * CVideoEncoder::getMP4AuxSDPLine(int rtp_pt)
{
	if (NULL == m_pCodecCtx || m_pCodecCtx->extradata_size == 0)
	{
		return NULL;
	}

	char const* fmtpFmt =
		"a=fmtp:%d "
		"profile-level-id=%d;"
		"config=";
	uint32 fmtpFmtSize = strlen(fmtpFmt)
		+ 3 /* max char len */
		+ 3 /* max char len */
		+ 2*m_pCodecCtx->extradata_size; /* 2*, because each byte prints as 2 chars */

	char* fmtp = new char[fmtpFmtSize+1];
	memset(fmtp, 0, fmtpFmtSize+1);

	sprintf(fmtp, fmtpFmt, rtp_pt, 1);
	char* endPtr = &fmtp[strlen(fmtp)];
	for (int i = 0; i < m_pCodecCtx->extradata_size; ++i)
	{
		sprintf(endPtr, "%02X", m_pCodecCtx->extradata[i]);
		endPtr += 2;
	}

	return fmtp;
}

char * CVideoEncoder::getAuxSDPLine(int rtp_pt)
{
	if (m_nCodecId == AV_CODEC_ID_H264)
	{
		return getH264AuxSDPLine(rtp_pt);
	}
	else if (m_nCodecId == AV_CODEC_ID_MPEG4)
	{
		return getMP4AuxSDPLine(rtp_pt);
	}
	else if (m_nCodecId == AV_CODEC_ID_HEVC)
	{
		return getH265AuxSDPLine(rtp_pt);
	}

	return NULL;
}





