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
#include "file_demux.h"
#include "audio_encoder.h"
#include "video_encoder.h"
#include "media_format.h"
#include "base64.h"
#include "media_util.h"
#include "rtsp_cfg.h"
#include "h264.h"
#include "h265.h"

/*********************************************************************************/
extern RTSP_CFG g_rtsp_cfg;

/*********************************************************************************/

void AudioCallback(uint8 *data, int size, int nbsamples, void *pUserdata)
{
	CFileDemux * pDemux = (CFileDemux *)pUserdata;
	pDemux->dataCallback(data, size, DATA_TYPE_AUDIO, nbsamples, TRUE);
}

void VideoCallback(uint8 *data, int size, void *pUserdata)
{
	CFileDemux * pDemux = (CFileDemux *)pUserdata;
	pDemux->dataCallback(data, size, DATA_TYPE_VIDEO, 0, TRUE);
}

CFileDemux::CFileDemux(const char * filename)
{
	// 모든 멤버 변수를 안전한 값으로 초기화합니다.
	m_nAudioIndex = -1;
	m_nVideoIndex = -1;
	m_nDuration = 0;
	m_nCurPos = 0;
	m_nNalLength = 0;

	m_pFormatContext = NULL;
	m_pACodecCtx = NULL;
	m_pVCodecCtx = NULL;
	m_pFrame = NULL;

	m_pVideoEncoder = NULL;
	m_pAudioEncoder = NULL;

	m_bFirst = TRUE;
	m_pCallback = NULL;
	m_pUserdata = NULL;

	m_nLoopNums = 0;

	// 생성자에서 초기화 함수를 호출합니다.
	init(filename);
}

CFileDemux::~CFileDemux()
{
	// 소멸자에서 리소스 해제 함수를 호출합니다.
	uninit();
}

BOOL CFileDemux::init(const char * filename)
{
	// 1. 파일 열기
	if (avformat_open_input(&m_pFormatContext, filename, NULL, NULL) != 0)
	{
		log_print(LOG_ERR, "avformat_open_input failed, %s\r\n", filename);
		return FALSE;
	}

	// 2. 스트림 정보 파싱
	if (avformat_find_stream_info(m_pFormatContext, NULL) < 0)
	{
		log_print(LOG_ERR, "avformat_find_stream_info failed\r\n");
		uninit(); // 실패 시 리소스 정리
		return FALSE;
	}

	if (m_pFormatContext->duration != AV_NOPTS_VALUE)
	{
		m_nDuration = m_pFormatContext->duration;
	}

	// 3. 오디오 및 비디오 스트림 인덱스 찾기
	for (unsigned int i=0; i < m_pFormatContext->nb_streams; i++)
	{
		AVStream* stream = m_pFormatContext->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_nVideoIndex == -1)
		{
			m_nVideoIndex = i;
			if( m_nDuration < stream->duration ) m_nDuration = stream->duration;
		}
		else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_nAudioIndex == -1)
		{
			m_nAudioIndex = i;
			if( m_nDuration < stream->duration ) m_nDuration = stream->duration;
		}
	}

	m_nDuration /= 1000; // 마이크로초 -> 밀리초

	// 4. 오디오 코덱 컨텍스트 설정
	if( m_nAudioIndex != -1 )
	{
		AVCodecParameters* codecpar = m_pFormatContext->streams[m_nAudioIndex]->codecpar;
		const AVCodec* aCodec = avcodec_find_decoder(codecpar->codec_id);
		if (!aCodec) {
			log_print(LOG_ERR, "Audio decoder not found\n");
			uninit(); return FALSE;
		}

		m_pACodecCtx = avcodec_alloc_context3(aCodec);
		if (!m_pACodecCtx) {
			log_print(LOG_ERR, "Failed to alloc audio codec context\n");
			uninit(); return FALSE;
		}

		if( avcodec_parameters_to_context(m_pACodecCtx, codecpar) < 0 ) {
			log_print(LOG_ERR, "Failed to copy audio codec parameters to context\n");
			uninit(); return FALSE;
		}
	}

	// 5. 비디오 코덱 컨텍스트 설정
	if( m_nVideoIndex != -1 )
	{
		AVCodecParameters* codecpar = m_pFormatContext->streams[m_nVideoIndex]->codecpar;
		const AVCodec* vCodec = avcodec_find_decoder(codecpar->codec_id);
		if (!vCodec) {
			log_print(LOG_ERR, "Video decoder not found\n");
			uninit(); return FALSE;
		}

		m_pVCodecCtx = avcodec_alloc_context3(vCodec);
		if (!m_pVCodecCtx) {
			log_print(LOG_ERR, "Failed to alloc video codec context\n");
			uninit(); return FALSE;
		}

		if( avcodec_parameters_to_context(m_pVCodecCtx, codecpar) < 0 ) {
			log_print(LOG_ERR, "Failed to copy video codec parameters to context\n");
			uninit(); return FALSE;
		}

		if (m_pVCodecCtx->codec_id == AV_CODEC_ID_H264) {
			if (m_pVCodecCtx->extradata && m_pVCodecCtx->extradata_size > 8 && m_pVCodecCtx->extradata[0] == 1) {
				m_nNalLength = (m_pVCodecCtx->extradata[4] & 0x03) + 1;
			}
		}
		else if (m_pVCodecCtx->codec_id == AV_CODEC_ID_HEVC) {
			if (m_pVCodecCtx->extradata && m_pVCodecCtx->extradata_size > 22) {
				// HEVC extradata 파싱은 복잡하므로 NAL length size만 간단히 파악
				m_nNalLength = ((m_pVCodecCtx->extradata[21] & 0x03) + 1);
			}
		}
	}

	// 6. 재사용할 프레임 할당
	m_pFrame = av_frame_alloc();
	if (!m_pFrame) {
		log_print(LOG_ERR, "av_frame_alloc failed\n");
		uninit();
		return FALSE;
	}

	return TRUE;
}

void CFileDemux::uninit()
{
	flushVideo();
	flushAudio();

	// 할당된 코덱 컨텍스트 해제
	if (m_pACodecCtx) {
		avcodec_free_context(&m_pACodecCtx);
	}

	if (m_pVCodecCtx) {
		avcodec_free_context(&m_pVCodecCtx);
	}

	// 할당된 프레임 해제
	if (m_pFrame) {
		av_frame_free(&m_pFrame);
	}

	// 인코더가 생성되었다면 해제
	if (m_pVideoEncoder) {
		delete m_pVideoEncoder;
		m_pVideoEncoder = NULL;
	}

	if (m_pAudioEncoder) {
		delete m_pAudioEncoder;
		m_pAudioEncoder = NULL;
	}

	// 포맷 컨텍스트 해제
	if (m_pFormatContext) {
		avformat_close_input(&m_pFormatContext);
	}
}

int CFileDemux::getWidth()
{
	// 비디오 코덱 컨텍스트가 유효하면 너비(width)를 반환하고, 아니면 0을 반환합니다.
	return (m_pVCodecCtx) ? m_pVCodecCtx->width : 0;
}

int CFileDemux::getHeight()
{
	// 비디오 코덱 컨텍스트가 유효하면 높이(height)를 반환하고, 아니면 0을 반환합니다.
	return (m_pVCodecCtx) ? m_pVCodecCtx->height : 0;
}

/**
 * @brief Fixed getFramerate logic to correctly identify file's FPS.
 * MP4 containers often use avg_frame_rate for precise timing.
 */
int CFileDemux::getFramerate()
{
    int framerate = 30; // Global default
    if(m_nVideoIndex != -1)
    {
        AVStream* st = m_pFormatContext->streams[m_nVideoIndex];

        // Priority 1: avg_frame_rate (Most accurate for files)
        AVRational fr = st->avg_frame_rate;

        // Priority 2: r_frame_rate (Guessed from timestamps if avg is 0)
        if(fr.num == 0) fr = st->r_frame_rate;

        if(fr.den > 0)
        {
            // Use rounding (+0.5) to ensure 29.97 becomes 30, not 29.
            framerate = static_cast<int>(av_q2d(fr) + 0.5);
        }
    }
    return (framerate > 0) ? framerate : 30;
}

int CFileDemux::getSamplerate()
{
	// 오디오 코덱 컨텍스트가 유효하면 샘플레이트를 반환하고, 아니면 기본값 8000을 반환합니다.
	return (m_pACodecCtx) ? m_pACodecCtx->sample_rate : 8000;
}

int CFileDemux::getChannels()
{
	if (m_pACodecCtx)
	{
		// FFmpeg 버전에 따라 올바른 API를 사용하도록 전처리기(Preprocessor)를 사용합니다.
		// libavutil 57.28.100 (FFmpeg 5.0) 이후 버전부터 ch_layout이 안정화되었습니다.
		#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
		// FFmpeg 5.0 이상 버전의 경우 (사용자께서 목표로 하시는 6.1.1 포함)
		return m_pACodecCtx->ch_layout.nb_channels;
		#else
		// 그 이전 버전의 FFmpeg 헤더가 포함된 경우 (레거시 호환성)
		return m_pACodecCtx->channels;
		#endif
	}

	return 2; // 코덱 컨텍스트가 없을 경우 기본값 2를 반환
}

BOOL CFileDemux::setAudioFormat(int codec, int samplerate, int channels, int bitrate)
{
	// 오디오 스트림이 없으면 아무 작업도 하지 않습니다.
	if (m_nAudioIndex == -1)
	{
		return FALSE;
	}

	// 원본 코드는 항상 재인코딩을 하도록 되어 있었으므로, 해당 로직을 유지합니다.
	// 특정 조건(예: 코덱, 샘플레이트, 채널이 다를 경우)에만 재인코딩하려면 아래 if문을 수정하면 됩니다.
	if (TRUE)
	{
		// 1. 디코더 열기: 패킷을 프레임으로 디코딩하기 위해 코덱을 엽니다.
		const AVCodec* pCodec = avcodec_find_decoder(m_pACodecCtx->codec_id);
		if (pCodec == NULL)
		{
			log_print(LOG_ERR, "avcodec_find_decoder failed, %d\r\n", m_pACodecCtx->codec_id);
			return FALSE;
		}
		if (avcodec_open2(m_pACodecCtx, pCodec, NULL) != 0)
		{
			log_print(LOG_ERR, "avcodec_open2 failed, audio decoder\r\n");
			return FALSE;
		}

		// 2. 재인코딩을 위한 인코더 생성 및 초기화
		m_pAudioEncoder = new CAudioEncoder;
		if (NULL == m_pAudioEncoder)
		{
			return FALSE;
		}

		// 인코더에 필요한 파라미터 설정 (원본 포맷 -> 목표 포맷)
		AudioEncoderParam params = {0};
		#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
		params.SrcChannels = m_pACodecCtx->ch_layout.nb_channels;
		#else
		params.SrcChannels = m_pACodecCtx->channels;
		#endif
		params.SrcSamplefmt = m_pACodecCtx->sample_fmt;
		params.SrcSamplerate = m_pACodecCtx->sample_rate;
		params.DstChannels = channels;
		params.DstSamplefmt = AV_SAMPLE_FMT_S16; // 예제에서는 S16 포맷으로 고정
		params.DstSamplerate = samplerate;
		params.DstBitrate = bitrate;
		params.DstCodec = codec;

		if (FALSE == m_pAudioEncoder->init(&params))
		{
			delete m_pAudioEncoder;
			m_pAudioEncoder = NULL;
			return FALSE;
		}

		// 인코딩된 데이터를 받을 콜백 함수 등록
		m_pAudioEncoder->addCallback(AudioCallback, this);
	}

	return TRUE;
}

BOOL CFileDemux::setVideoFormat(int codec, int width, int height, int framerate, int bitrate)
{
	if (m_nVideoIndex == -1)
	{
		return FALSE;
	}

	// 코덱 ID나 해상도가 다를 경우에만 재인코딩을 수행합니다.
	if (m_pVCodecCtx->codec_id != CVideoEncoder::toAVCodecID(codec) ||
		 m_pVCodecCtx->width != width ||
		 m_pVCodecCtx->height != height)
	{
		// 1. 디코더 열기
		const AVCodec * pCodec = avcodec_find_decoder(m_pVCodecCtx->codec_id);
		if (pCodec == NULL)
		{
			log_print(LOG_ERR, "avcodec_find_decoder failed, %d\r\n", m_pVCodecCtx->codec_id);
			return FALSE;
		}

		if (avcodec_open2(m_pVCodecCtx, pCodec, NULL) != 0)
		{
			log_print(LOG_ERR, "avcodec_open2 failed, video decoder\r\n");
			return FALSE;
		}

		// 2. 재인코딩을 위한 인코더 생성 및 초기화
		m_pVideoEncoder = new CVideoEncoder;
		if (NULL == m_pVideoEncoder)
		{
			return FALSE;
		}

		VideoEncoderParam params = {0};
		params.SrcWidth = m_pVCodecCtx->width;
		params.SrcHeight = m_pVCodecCtx->height;
		params.SrcPixFmt = m_pVCodecCtx->pix_fmt;
		params.DstCodec = codec;
		params.DstWidth = width;
		params.DstHeight = height;
		params.DstFramerate = framerate;
		params.DstBitrate = bitrate;

		if (FALSE == m_pVideoEncoder->init(&params))
		{
			delete m_pVideoEncoder;
			m_pVideoEncoder = NULL;
			return FALSE;
		}

		m_pVideoEncoder->addCallback(VideoCallback, this);
	}
	// H.264나 HEVC 코덱이고 재인코딩이 필요 없을 때, MP4 컨테이너 형식을 Annex B 스트림 형식으로 변환하기 위한 비트스트림 필터 설정
	// (RTSP 등 스트리밍에 필요한 작업)
	// else if (m_pVCodecCtx->codec_id == AV_CODEC_ID_H264 || m_pVCodecCtx->codec_id == AV_CODEC_ID_HEVC)
	// {
	// 	const char* filter_name = (m_pVCodecCtx->codec_id == AV_CODEC_ID_H264) ? "h264_mp4toannexb" : "hevc_mp4toannexb";
	// 	const AVBitStreamFilter* bsfc = av_bsf_get_by_name(filter_name);
	// 	if (bsfc)
	// 	{
	// 		AVBSFContext *bsf_ctx = NULL;
	// 		if (av_bsf_alloc(bsfc, &bsf_ctx) < 0) return FALSE;

	// 		avcodec_parameters_from_context(bsf_ctx->par_in, m_pVCodecCtx);
	// 		av_bsf_init(bsf_ctx);
	// 		avcodec_parameters_to_context(m_pVCodecCtx, bsf_ctx->par_out);

	// 		av_bsf_free(&bsf_ctx);
	// 	}
	// }

	return TRUE;
}

void CFileDemux::videoRecodec(AVPacket* pkt)
{
	// 1. 압축된 데이터(AVPacket)를 디코더에 보냅니다.
	// pkt가 NULL이면 디코더에 캐시된 프레임을 모두 출력하라는 신호(flushing)입니다.
	int ret = avcodec_send_packet(m_pVCodecCtx, pkt);
	if (ret < 0) {
		log_print(LOG_ERR, "Error sending a packet for decoding video\n");
		return;
	}

	while (ret >= 0) {
		// 2. 디코딩된 데이터(AVFrame)를 받습니다.
		// EAGAIN: 지금 당장 출력할 프레임이 없으니, 새로운 패킷을 더 넣어야 함
		// AVERROR_EOF: 스트림의 끝에 도달하여 더 이상 출력할 프레임이 없음
		ret = avcodec_receive_frame(m_pVCodecCtx, m_pFrame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return;
		} else if (ret < 0) {
			log_print(LOG_ERR, "Error during decoding video\n");
			return;
		}

		// 3. 디코딩된 프레임(m_pFrame)을 인코더로 보내 재인코딩합니다.
		m_pVideoEncoder->encode(m_pFrame);

		// 프레임의 참조를 해제하여 다음 데이터를 받을 수 있도록 준비합니다.
		av_frame_unref(m_pFrame);
	}
}

void CFileDemux::audioRecodec(AVPacket* pkt)
{
	// 비디오 재인코딩 과정과 동일합니다.
	int ret = avcodec_send_packet(m_pACodecCtx, pkt);
	if (ret < 0) {
		log_print(LOG_ERR, "Error sending a packet for decoding audio\n");
		return;
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(m_pACodecCtx, m_pFrame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return;
		} else if (ret < 0) {
			log_print(LOG_ERR, "Error during decoding audio\n");
			return;
		}

		m_pAudioEncoder->encode(m_pFrame);
		av_frame_unref(m_pFrame);
	}
}

BOOL CFileDemux::readFrame()
{
	AVPacket pkt;
	int rret = av_read_frame(m_pFormatContext, &pkt);

	// 파일의 끝(AVERROR_EOF)에 도달한 경우
	if (rret < 0) {
		if (rret == AVERROR_EOF) {
			// 파일 반복 재생 로직 (g_rtsp_cfg.loop_nums에 따라)
			if (++m_nLoopNums >= g_rtsp_cfg.loop_nums) return FALSE;

			int64_t ts = (m_pFormatContext->streams[0]->start_time != AV_NOPTS_VALUE) ? m_pFormatContext->streams[0]->start_time : 0;
			if (av_seek_frame(m_pFormatContext, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) return FALSE;

			// seek 후 첫 프레임을 다시 읽어옵니다.
			rret = av_read_frame(m_pFormatContext, &pkt);
			if (rret < 0) return FALSE;
		} else {
			return FALSE; // 그 외의 읽기 오류
		}
	}

	// 읽어온 패킷이 비디오 스트림인 경우
	if (pkt.stream_index == m_nVideoIndex)
	{
		// 현재 재생 위치(Timestamp) 계산
		AVRational tb = m_pFormatContext->streams[m_nVideoIndex]->time_base;
		int64_t pts = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
		if (pts != AV_NOPTS_VALUE) {
			m_nCurPos = av_rescale_q(pts, tb, {1, 1000}); // 밀리초 단위로 변환
		}

		if (m_pVideoEncoder == NULL)
		{
			// 재인코딩이 필요 없는 경우, 패킷을 그대로 콜백으로 전달
			// H.264/HEVC의 경우 MP4 형식(length-prefix)을 Annex B 형식(start-code)으로 변환해주는 작업이 필요할 수 있음
			if ((m_pVCodecCtx->codec_id == AV_CODEC_ID_H264 || m_pVCodecCtx->codec_id == AV_CODEC_ID_HEVC) && m_nNalLength > 0)
			{
				// SPS, PPS 등 헤더 정보가 필요한 스트림의 경우, 첫 프레임 전에 헤더를 먼저 전달
				if (m_bFirst) {
					m_bFirst = FALSE;
					if (m_pVCodecCtx->extradata_size > 0) {
						dataCallback(m_pVCodecCtx->extradata, m_pVCodecCtx->extradata_size, DATA_TYPE_VIDEO, 0, FALSE);
					}
				}

				// 원본 코드의 NAL 파싱 로직 (length -> start code)
				uint8 * data_ptr = pkt.data;
				int size_left = pkt.size;
				while (size_left >= m_nNalLength) {
					uint32_t len = 0;
					for (int i = 0; i < m_nNalLength; ++i) len = (len << 8) | data_ptr[i];

					if (len > static_cast<uint32_t>(size_left - m_nNalLength) || len <= 0) break;

					// NAL 유닛 길이 정보를 시작 코드로 변경 (0x00000001)
					uint8 start_code[4] = {0, 0, 0, 1};
					memcpy(data_ptr, start_code, 4);

					dataCallback(data_ptr, len + m_nNalLength - 4, DATA_TYPE_VIDEO, 0, (size_left - m_nNalLength - len > 0) ? FALSE : TRUE);

					data_ptr += len + m_nNalLength;
					size_left -= len + m_nNalLength;
				}
			}
			else
			{
				dataCallback(pkt.data, pkt.size, DATA_TYPE_VIDEO, 0, TRUE);
			}
		} else {
			// 재인코딩이 필요한 경우, videoRecodec 함수 호출
			videoRecodec(&pkt);
		}
	}
	// 읽어온 패킷이 오디오 스트림인 경우
	else if (pkt.stream_index == m_nAudioIndex)
	{
		AVRational tb = m_pFormatContext->streams[m_nAudioIndex]->time_base;
		int64_t pts = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
		if (pts != AV_NOPTS_VALUE) {
			m_nCurPos = av_rescale_q (pts, tb, {1, 1000}); // 밀리초 단위로 변환
		}

		if (m_pAudioEncoder == NULL)
		{
			// 재인코딩이 필요 없으면 패킷 그대로 전달
			dataCallback(pkt.data, pkt.size, DATA_TYPE_AUDIO, 0, TRUE);
		}
		else
		{
			// 재인코딩이 필요하면 audioRecodec 함수 호출
			audioRecodec(&pkt);
		}
	}

	// 패킷 데이터의 참조를 해제합니다.
	av_packet_unref(&pkt);
	return TRUE;
}

BOOL CFileDemux::seekStream(double pos)
{
	// 유효하지 않은 위치로는 탐색하지 않습니다.
	if (pos < 0 || m_pFormatContext == NULL)
	{
		return FALSE;
	}

	// 기본 스트림을 기준으로 탐색합니다. 비디오가 있으면 비디오 기준, 없으면 오디오 기준.
	int stream_index = -1;
	if (m_nVideoIndex >= 0)
	{
		stream_index = m_nVideoIndex;
	}
	else if (m_nAudioIndex >= 0)
	{
		stream_index = m_nAudioIndex;
	}
	else
	{
		return FALSE; // 탐색 기준이 될 스트림이 없음
	}

	// 1. 탐색할 위치(초 단위)를 스트림의 타임베이스에 맞게 변환합니다.
	int64_t seek_pos = av_rescale_q(
		static_cast<int64_t>(pos * AV_TIME_BASE),
		{1, AV_TIME_BASE},
		m_pFormatContext->streams[stream_index]->time_base
	);

	// 2. av_seek_frame 함수를 호출하여 해당 위치의 키프레임(keyframe)으로 이동합니다.
	// AVSEEK_FLAG_BACKWARD 플래그는 지정된 타임스탬프보다 이전의 가장 가까운 키프레임을 찾도록 합니다.
	if (av_seek_frame(m_pFormatContext, stream_index, seek_pos, AVSEEK_FLAG_BACKWARD) < 0)
	{
		return FALSE;
	}

	// 3. 디코더 버퍼를 비워줍니다. (탐색 후에는 이전 데이터가 남아있으면 안 됨)
	if (m_pACodecCtx)
	{
		avcodec_flush_buffers(m_pACodecCtx);
	}
	if (m_pVCodecCtx)
	{
		avcodec_flush_buffers(m_pVCodecCtx);
	}

	// 현재 위치를 업데이트합니다.
	m_nCurPos = static_cast<int64>(pos * 1000);

	return TRUE;
}

void CFileDemux::setCallback(DemuxCallback pCallback, void * pUserdata)
{
	// 외부에서 등록한 콜백 함수 포인터와 사용자 데이터를 멤버 변수에 저장합니다.
	m_pCallback = pCallback;
	m_pUserdata = pUserdata;
}

void CFileDemux::dataCallback(uint8 * data, int size, int type, int nbsamples, BOOL waitnext)
{
	// 콜백 함수가 등록되어 있으면, 처리된 데이터를 콜백 함수를 통해 외부로 전달합니다.
	if (m_pCallback)
	{
		m_pCallback(data, size, type, nbsamples, waitnext, m_pUserdata);
	}
}

#include "base64.h" // base64 인코딩을 위해 필요 (프로젝트에 해당 파일이 있어야 함)

// ...

char * CFileDemux::getH264AuxSDPLine(int rtp_pt)
{
	uint8 * sps = NULL; uint32 spsSize = 0;
	uint8 * pps = NULL; uint32 ppsSize = 0;

	if (NULL == m_pVCodecCtx || m_pVCodecCtx->extradata == NULL || m_pVCodecCtx->extradata_size <= 8)
	{
		return NULL;
	}

	// extradata에서 SPS와 PPS를 파싱합니다.
	uint8 *r, *end = m_pVCodecCtx->extradata + m_pVCodecCtx->extradata_size;
	r = avc_find_startcode(m_pVCodecCtx->extradata, end);

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

	if (NULL == sps || spsSize == 0 || NULL == pps || ppsSize == 0)
	{
		return NULL;
	}

	// SPS에서 profile-level-id 값을 추출합니다.
	uint8* spsWEB = new uint8[spsSize];
	uint32 spsWEBSize = remove_emulation_bytes(spsWEB, spsSize, sps, spsSize);
	if (spsWEBSize < 4)
	{
		delete[] spsWEB;
		return NULL;
	}
	uint32 profileLevelId = (spsWEB[1]<<16) | (spsWEB[2]<<8) | spsWEB[3];
	delete[] spsWEB;

	// SPS와 PPS를 Base64로 인코딩합니다.
	char* sps_base64 = new char[spsSize * 2];
	char* pps_base64 = new char[ppsSize * 2];
	base64_encode(sps, spsSize, sps_base64, spsSize * 2 + 1);
	base64_encode(pps, ppsSize, pps_base64, ppsSize * 2 + 1);

	// SDP a=fmtp 라인을 포맷에 맞게 생성합니다.
	char const* fmtpFmt = "a=fmtp:%d packetization-mode=1;profile-level-id=%06X;sprop-parameter-sets=%s,%s";
	uint32 fmtpFmtSize = strlen(fmtpFmt) + 3 + 6 + strlen(sps_base64) + strlen(pps_base64) + 2;
	char* fmtp = new char[fmtpFmtSize];
	sprintf(fmtp, fmtpFmt, rtp_pt, profileLevelId, sps_base64, pps_base64);

	delete[] sps_base64;
	delete[] pps_base64;
	return fmtp;
}

char * CFileDemux::getH265AuxSDPLine(int rtp_pt)
{
	// H.265의 경우 VPS, SPS, PPS가 필요합니다. 로직은 H.264와 유사합니다.
	uint8* vps = NULL; uint32 vpsSize = 0;
	uint8* sps = NULL; uint32 spsSize = 0;
	uint8* pps = NULL; uint32 ppsSize = 0;

	if (NULL == m_pVCodecCtx || m_pVCodecCtx->extradata == NULL || m_pVCodecCtx->extradata_size < 23)
	{
		return NULL;
	}

	uint8 *r, *end = m_pVCodecCtx->extradata + m_pVCodecCtx->extradata_size;
	r = avc_find_startcode(m_pVCodecCtx->extradata, end);

	while (r < end)
	{
		uint8 *r1;
		while (!*(r++));
		r1 = avc_find_startcode(r, end);

		int nal_type = (r[0] >> 1) & 0x3F;
		if (HEVC_NAL_VPS == nal_type) { vps = r; vpsSize = r1 - r; }
		else if (HEVC_NAL_PPS == nal_type) { pps = r; ppsSize = r1 - r; }
		else if (HEVC_NAL_SPS == nal_type) { sps = r; spsSize = r1 - r; }

		r = r1;
	}

	if (NULL == vps || vpsSize == 0 || NULL == sps || spsSize == 0 || NULL == pps || ppsSize == 0)
	{
		return NULL;
	}

	// VPS, SPS, PPS를 Base64로 인코딩합니다.
	char* sprop_vps = new char[vpsSize*2];
	char* sprop_sps = new char[spsSize*2];
	char* sprop_pps = new char[ppsSize*2];

	base64_encode(vps, vpsSize, sprop_vps, vpsSize*2+1);
	base64_encode(sps, spsSize, sprop_sps, spsSize*2+1);
	base64_encode(pps, ppsSize, sprop_pps, ppsSize*2+1);

	// H.265 SDP a=fmtp 라인을 생성합니다.
	char const* fmtpFmt = "a=fmtp:%d sprop-vps=%s;sprop-sps=%s;sprop-pps=%s";
	uint32 fmtpFmtSize = strlen(fmtpFmt) + 3 + strlen(sprop_vps) + strlen(sprop_sps) + strlen(sprop_pps) + 4;
	char* fmtp = new char[fmtpFmtSize];
	sprintf(fmtp, fmtpFmt, rtp_pt, sprop_vps, sprop_sps, sprop_pps);

	delete[] sprop_vps;
	delete[] sprop_sps;
	delete[] sprop_pps;
	return fmtp;
}

char * CFileDemux::getMP4AuxSDPLine(int rtp_pt)
{
	if (NULL == m_pVCodecCtx || m_pVCodecCtx->extradata == NULL || m_pVCodecCtx->extradata_size == 0)
	{
		return NULL;
	}

	char const* fmtpFmt = "a=fmtp:%d profile-level-id=1;config=";
	uint32 fmtpFmtSize = strlen(fmtpFmt) + 3 + 2*m_pVCodecCtx->extradata_size + 1;
	char* fmtp = new char[fmtpFmtSize];
	sprintf(fmtp, fmtpFmt, rtp_pt);

	char* endPtr = &fmtp[strlen(fmtp)];
	for (int i = 0; i < m_pVCodecCtx->extradata_size; ++i)
	{
		sprintf(endPtr, "%02X", m_pVCodecCtx->extradata[i]);
		endPtr += 2;
	}

	return fmtp;
}

char * CFileDemux::getAACAuxSDPLine(int rtp_pt)
{
	if (NULL == m_pACodecCtx || m_pACodecCtx->extradata == NULL || m_pACodecCtx->extradata_size == 0)
	{
		return NULL;
	}

	char const* fmtpFmt = "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=";
	uint32 fmtpFmtSize = strlen(fmtpFmt) + 3 + 2*m_pACodecCtx->extradata_size + 1;
	char* fmtp = new char[fmtpFmtSize];
	sprintf(fmtp, fmtpFmt, rtp_pt);

	char* endPtr = &fmtp[strlen(fmtp)];
	for (int i = 0; i < m_pACodecCtx->extradata_size; ++i)
	{
		sprintf(endPtr, "%02X", m_pACodecCtx->extradata[i]);
		endPtr += 2;
	}

	return fmtp;
}

char * CFileDemux::getVideoAuxSDPLine(int rtp_pt)
{
	if (m_pVideoEncoder)
	{
		return m_pVideoEncoder->getAuxSDPLine(rtp_pt);
	}

	if (m_pVCodecCtx == NULL) return NULL;

	if (m_pVCodecCtx->codec_id == AV_CODEC_ID_H264)
	{
		return getH264AuxSDPLine(rtp_pt);
	}
	else if (m_pVCodecCtx->codec_id == AV_CODEC_ID_MPEG4)
	{
		return getMP4AuxSDPLine(rtp_pt);
	}
	else if (m_pVCodecCtx->codec_id == AV_CODEC_ID_HEVC)
	{
		return getH265AuxSDPLine(rtp_pt);
	}

	return NULL;
}

char * CFileDemux::getAudioAuxSDPLine(int rtp_pt)
{
	if (m_pAudioEncoder)
	{
		return m_pAudioEncoder->getAuxSDPLine(rtp_pt);
	}

	if (m_pACodecCtx == NULL) return NULL;

	if (m_pACodecCtx->codec_id == AV_CODEC_ID_AAC)
	{
		return getAACAuxSDPLine(rtp_pt);
	}

	return NULL;
}

void CFileDemux::flushVideo()
{
	// 재인코딩이 설정된 경우에만 플러시가 필요합니다.
	if (m_pVideoEncoder == NULL || m_pVCodecCtx == NULL)
	{
		return;
	}

	// videoRecodec 함수에 NULL 패킷을 보내 디코더를 플러시(flush)합니다.
	videoRecodec(NULL);
}

void CFileDemux::flushAudio()
{
	// 재인코딩이 설정된 경우에만 플러시가 필요합니다.
	if (m_pAudioEncoder == NULL || m_pACodecCtx == NULL)
	{
		return;
	}

	// audioRecodec 함수에 NULL 패킷을 보내 디코더를 플러시(flush)합니다.
	audioRecodec(NULL);
}
