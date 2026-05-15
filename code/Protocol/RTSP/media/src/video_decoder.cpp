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

#include "video_decoder.h"
#include "MgenLogger.h"
#include "h264.h"
#include "h265.h"

#include <algorithm> // search
#include <cmath>     // log2, ceil
#include <sstream>   // std::stringstream
#include <cstring>   // memcpy, memcmp
#include <vector>
#include <string>

// ======== STATIC FUNCTIONS ==========================================================================================================
// Annex B 형식 데이터에서 다음 NAL 유닛의 시작 위치와 길이를 찾음
// data: 검색할 데이터 버퍼
// size: 데이터 버퍼의 크기
// offset: 검색 시작 위치 (함수 호출 후 다음 NAL 유닛 시작 위치로 업데이트됨)
// nal_start_code_len: 찾은 시작 코드의 길이 (3 또는 4)
// returns: 찾은 NAL 유닛의 전체 길이 (시작 코드 포함), 못 찾으면 0 또는 음수
static int find_next_nal_unit( const uint8_t *data, int size, int *offset, int *nal_start_code_len )
{
	int i = *offset;
	// 시작 코드 (00 00 01 또는 00 00 00 01) 찾기
	while( i + 3 < size )
	{
		if( data[i] == 0 && data[i+1] == 0 )
		{
			// 00 00 01
			if( data[i+2] == 1 )
			{
				*nal_start_code_len = 3;
				break;
			}

			// 00 00 00 01
			if( i + 3 < size && data[i+2] == 0 && data[i+3] == 1 )
			{
				*nal_start_code_len = 4;
				break;
			}
		}
		i++;
	}

	// 시작 코드를 못 찾음
	if( i + 3 >= size )
		return 0;

	int start_pos = i;        // 현재 NAL 유닛 시작 위치
	i += *nal_start_code_len; // 다음 검색 시작 위치

	// 다음 시작 코드 찾기 (현재 NAL 유닛의 끝을 알기 위해)
	while( i + 3 < size )
	{
		if( data[i] == 0 && data[i+1] == 0 )
		{
			if( data[i+2] == 1 || (i + 3 < size && data[i+2] == 0 && data[i+3] == 1 ) )
			{
				*offset = i; // 다음 NAL 유닛 시작 위치 업데이트
				return i - start_pos; // 현재 NAL 유닛 길이 반환
			}
		}
		i++;
	}

	// 다음 시작 코드를 못 찾으면 버퍼 끝까지가 현재 NAL 유닛임
	*offset = size;
	return size - start_pos;
}

// 시작 코드를 제외한 NAL 유닛 데이터와 타입을 반환하는 함수
// nal_unit_data: 시작 코드 포함 전체 NAL 유닛 데이터
// nal_unit_len: 전체 NAL 유닛 길이
// start_code_len: 시작 코드 길이
// nal_type: [출력] NAL 유닛 타입
// returns: 시작 코드 제외한 실제 데이터 포인터, 실패 시 nullptr
static const uint8_t* get_nal_data_and_type
(
	const uint8_t* nal_unit_data, int nal_unit_len, int start_code_len, int* nal_type, AVCodecID codec_id
)
{
	if( nal_unit_len <= start_code_len )
		return nullptr;

	const uint8_t* data = nal_unit_data + start_code_len;
	if( codec_id == AV_CODEC_ID_H264 )
	{
		*nal_type = data[0] & 0x1F;
	}
	else if( codec_id == AV_CODEC_ID_H265 )
	{
		*nal_type = (data[0] >> 1) & 0x3F;
	}
	else
	{
		*nal_type = -1; // Unknown
		return nullptr;
	}
	return data;
}

static int AVCodecIDToDefineCodecID( AVCodecID avCodecId )
{
	switch( avCodecId ) {
	case AV_CODEC_ID_H264:  return VIDEO_CODEC_H264;
	case AV_CODEC_ID_H265:  return VIDEO_CODEC_H265;
	case AV_CODEC_ID_MJPEG: return VIDEO_CODEC_JPEG;
	case AV_CODEC_ID_MPEG4: return VIDEO_CODEC_MP4;
	default:                return VIDEO_CODEC_NONE;
	}
}
// ======== STATIC FUNCTIONS ==========================================================================================================

CVideoDecoder::CVideoDecoder()
{
	// m_pTryDecodeFrame는 여기서 할당
	if( m_pTryDecodeFrame = av_frame_alloc(); !m_pTryDecodeFrame ){
		MLOG_ERROR("CVideoDecoder() - Failed to allocate m_pTryDecodeFrame!");
		// 생성자에서 실패를 알릴 더 좋은 방법이 필요할 수 있음 (예: 예외)
	}

	// [수정] SPS/PPS 구조체 초기화 (기본값으로)
	m_current_sps = {}; // H264SPSInfo의 기본 생성자 호출 (valid=false)
	m_current_pps = {}; // H264PPSInfo의 기본 생성자 호출 (valid=false)
}

CVideoDecoder::~CVideoDecoder()
{
	uninit();
}

void CVideoDecoder::uninit()
{
	flush();

	// 코덱 컨텍스트 해제
	if( m_pContext )
	{
		avcodec_close(m_pContext);
		avcodec_free_context(&m_pContext);
		m_pContext = nullptr;
	}

	// 프레임 해제 (delPFrame 플래그에 따라)
	if( delPFrame && m_pFrame )
	{
	    av_frame_free(&m_pFrame);
		m_pFrame = nullptr;
	}

	// 플러시 프레임 해제
	if (m_pFlushFrame)
	{
		av_frame_free(&m_pFlushFrame);
		m_pFlushFrame = nullptr;
	}

	// 복사용 큐 해제
	if( m_pTryDecodeFrame )
	{
		av_frame_free( &m_pTryDecodeFrame );
		m_pTryDecodeFrame = nullptr;
	}

	// [수정] SPS/PPS 구조체 리셋
	m_current_sps = {};
	m_current_pps = {};

	// 상태 플래그 리셋
	m_bInited   = FALSE;
	m_nCodec    = VIDEO_CODEC_NONE;
	m_pCodec    = nullptr;
	m_pCallback = nullptr;
	m_pUserdata = nullptr;
}

bool CVideoDecoder::setDecoderInitContext( void )
{
	if( m_pCodec == nullptr || m_pContext == nullptr ) {
		MLOG_ERROR( "%s(), Codec or Context is NULL", __FUNCTION__ );
		return false;
	}

	// Skip b-frame decode
	// for improve performance, trade-off 30fps -> 20fps
	// m_pContext->skip_frame = AVDISCARD_BIDIR;

	#if 0
	if (VIDEO_CODEC_H264 == codec)
		m_pContext->flags2 |= AV_CODEC_FLAG2_CHUNKS;

	if (m_pCodec->capabilities & AV_CODEC_CAP_TRUNCATED)
		m_pContext->flags |= AV_CODEC_FLAG_TRUNCATED;
	#endif

	// 코덱이 프레임 기반 스레딩을 지원하는지 확인
	if( m_pCodec->capabilities & AV_CODEC_CAP_FRAME_THREADS )
	{
		// MLOG_INFO("Decoder Set Option : FFmpeg multi-threading");
		// FFmpeg가 시스템의 코어 수에 맞춰 최적의 스레드 수를 자동으로 결정하도록 설정
		m_pContext->thread_type  = FF_THREAD_FRAME;
		m_pContext->thread_count = 0;
	} else if (m_pCodec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
		// MLOG_INFO("Decoder Set Option : FFmpeg slice-threading");
		// 슬라이스 기반 스레딩 (차선책)
		m_pContext->thread_type  = FF_THREAD_SLICE;
		m_pContext->thread_count = 0;
	}

	// 지연을 유발하는 B-프레임 재정렬 등의 작업을 최소화하도록 유도
	m_pContext->flags |= AV_CODEC_FLAG_LOW_DELAY;

	return true;
}

// 구 버전 init 함수 (호환성 유지)
BOOL CVideoDecoder::init( int codec, int gpuId )
{
	switch ( codec )
	{
	case VIDEO_CODEC_H264:
		if ( gpuId >= 0 )
			m_pCodec = avcodec_find_decoder_by_name( "h264_cuvid" );
		else
			m_pCodec = avcodec_find_decoder( AV_CODEC_ID_H264 );
		break;

	case VIDEO_CODEC_H265:
		if ( gpuId >= 0 )
			m_pCodec = avcodec_find_decoder_by_name( "hevc_cuvid" );
		else
			m_pCodec = avcodec_find_decoder( AV_CODEC_ID_H265 );
		break;

	case VIDEO_CODEC_JPEG:
		m_pCodec = avcodec_find_decoder( AV_CODEC_ID_MJPEG );
		break;

	case VIDEO_CODEC_MP4:
		m_pCodec = avcodec_find_decoder( AV_CODEC_ID_MPEG4 );
		break;
	}

	if( m_pCodec == NULL ){
		MLOG_ERROR("%s(), m_pCodec is NULL", __FUNCTION__);
		return FALSE;
	}

	m_pContext = avcodec_alloc_context3(m_pCodec);
	if( NULL == m_pContext ){
		MLOG_ERROR("%s(), avcodec_alloc_context3 failed", __FUNCTION__);
		return FALSE;
	}

	if( setDecoderInitContext() == false ) {
		MLOG_ERROR( "%s(), setDecoderInitContext failed", __FUNCTION__ );
		avcodec_free_context( &m_pContext );
		return FALSE;
	}

	if( gpuId >= 0 )
	{
		char cGpuId[32];
		snprintf(cGpuId, sizeof(cGpuId), "%d", gpuId);

		int r = av_opt_set(m_pContext->priv_data, "gpu", cGpuId, 0);
		MLOG_INFO("video_decoder opt_set result is %d with given gpuId %d", r, gpuId);
	}

	if( avcodec_open2(m_pContext, m_pCodec, NULL) < 0 )
	{
		MLOG_ERROR("%s(), avcodec_open2 failed", __FUNCTION__);
		avcodec_free_context( &m_pContext );
		return FALSE;
	}

	m_pFrame = av_frame_alloc();
	if( NULL == m_pFrame )
	{
		MLOG_ERROR("%s(), av_frame_alloc failed", __FUNCTION__);
		avcodec_close( m_pContext );
		avcodec_free_context( &m_pContext );
		return FALSE;
	}

	m_pFlushFrame = av_frame_alloc();
	m_nCodec      = codec;
	m_bInited     = TRUE;

	return TRUE;
}

BOOL CVideoDecoder::init
(
	AVCodecID codecId, int width, int height, uint8_t* pExtraData, int nExtraDataSize, int gpuId, int camId
)
{
	const char* codecName = NULL;
    this->m_cam_id = camId;

    // -------------------------------------------------------------------------
    // 1. 코덱 이름 결정
    // -------------------------------------------------------------------------
    switch( codecId ) {
    case AV_CODEC_ID_H264: if( gpuId >= 0 ) codecName = "h264_cuvid"; break;
    case AV_CODEC_ID_H265: if( gpuId >= 0 ) codecName = "hevc_cuvid"; break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MPEG4: break;
    default:
        MLOG_ERROR( "CAM<%d>, Unsupported codec ID: %d", m_cam_id, codecId );
        return FALSE;
    }

    // -------------------------------------------------------------------------
    // 2. 코덱 및 컨텍스트 할당
    // -------------------------------------------------------------------------
    if( codecName ) m_pCodec = avcodec_find_decoder_by_name( codecName );
    else            m_pCodec = avcodec_find_decoder( codecId );

    if( m_pCodec == NULL ) {
        MLOG_ERROR( "CAM<%d>, m_pCodec is NULL (ID:%d, Name:%s)", m_cam_id, codecId, codecName ? codecName : "Auto" );
        return FALSE;
    }

    m_pContext = avcodec_alloc_context3( m_pCodec );
    if( NULL == m_pContext ) {
        MLOG_ERROR( "CAM<%d>, avcodec_alloc_context3 failed", m_cam_id );
        return FALSE;
    }

    m_pContext->codec_id = codecId;
    m_pContext->width    = width;
    m_pContext->height   = height;

    // -------------------------------------------------------------------------
    // 3. Extradata 처리 및 SPS/PPS 파싱 (정보 수집용 문자열 생성)
    // -------------------------------------------------------------------------
    std::string sps_summary = "";
    std::string pps_summary = "";

    if( pExtraData && nExtraDataSize > 0 )
    {
        m_pContext->extradata = (uint8_t*)av_malloc( static_cast<size_t>(nExtraDataSize) + AV_INPUT_BUFFER_PADDING_SIZE );
        if( !m_pContext->extradata ) {
            MLOG_ERROR("CAM<%d>, av_malloc for extradata failed", m_cam_id);
            avcodec_free_context(&m_pContext);
            return FALSE;
        }
        memcpy( m_pContext->extradata, pExtraData, nExtraDataSize );
        m_pContext->extradata_size = nExtraDataSize;

        // H.264 파싱 (로그 출력용 정보 수집만 수행)
        if( codecId == AV_CODEC_ID_H264 )
        {
            int current_offset = 0, nal_len = 0, sc_len = 0, found_nal_type = -1;
            const uint8_t* found_nal_data = nullptr;

            while( ( nal_len = find_next_nal_unit( pExtraData, nExtraDataSize, &current_offset, &sc_len ) ) > 0 )
            {
                int start_of_nal = current_offset - nal_len;
                found_nal_data   = get_nal_data_and_type( &pExtraData[start_of_nal], nal_len, sc_len, &found_nal_type, codecId );

                if (found_nal_data) {
                    if( found_nal_type == H264_NAL_SPS ) {
                        m_current_sps.valid = parse_sps( found_nal_data, nal_len - sc_len, m_current_sps );
                        if( m_current_sps.valid ) {
                            // 예: "Profile: 66, Level: 31" -> "Pro:66@L31" 처럼 간소화하거나 함수 활용
                            // 기존 함수 sps_info_to_string가 "Profile: 66..." 형태라면 그대로 사용하되 요약 로그에 포함
                            // 여기서는 예시로 요약된 스트링을 가정합니다. 필요시 함수 수정 필요.
                            // 간단하게 SPS 파싱 성공 여부만 체크하거나, 별도 요약 함수를 만드는 게 좋습니다.
                            // sps_summary = "SPS:Valid";
                            sps_summary = sps_info_to_string(m_current_sps); // 기존 함수가 너무 길다면 수정 필요
                        }
                    }
                    else if( found_nal_type == H264_NAL_PPS ) {
                        m_current_pps.valid = parse_pps( found_nal_data, nal_len - sc_len, m_current_pps );
                        if( m_current_pps.valid ) {
                            pps_summary = pps_info_to_string(m_current_pps);
                        }
                    }
                }
                if( current_offset >= nExtraDataSize ) break;
            }
        }
    }
    else if ( codecId == AV_CODEC_ID_H264 || codecId == AV_CODEC_ID_H265 ) {
        MLOG_ERROR("CAM<%d>, H.264/H.265 requires extradata (SPS/PPS) but NULL!", m_cam_id);
        avcodec_free_context(&m_pContext);
        return FALSE;
    }

    // -------------------------------------------------------------------------
    // 4. 디코더 설정 및 오픈
    // -------------------------------------------------------------------------
    if( setDecoderInitContext() == false ) {
        MLOG_ERROR( "%s(), setDecoderInitContext failed", __FUNCTION__ );
        avcodec_free_context( &m_pContext );
        return FALSE;
    }

    if( gpuId >= 0 ) {
		char cGpuId[32];
		snprintf(cGpuId, sizeof(cGpuId), "%d", gpuId);
        av_opt_set(m_pContext->priv_data, "gpu", cGpuId, 0);
    }

    if( avcodec_open2(m_pContext, m_pCodec, NULL ) < 0 )
    {
        MLOG_ERROR("CAM<%d> - [FATAL] avcodec_open2 failed! [%s] (Res:%dx%d, Extra:%d)",
                    m_cam_id,  m_pCodec ? m_pCodec->name : "NULL",
                    m_pContext->width, m_pContext->height, m_pContext->extradata_size );
        avcodec_free_context(&m_pContext);
        return FALSE;
    }

    // -------------------------------------------------------------------------
    // 5. 프레임 할당
    // -------------------------------------------------------------------------
    m_pFrame = av_frame_alloc();
    m_pFlushFrame = av_frame_alloc();
    if( !m_pFrame || !m_pFlushFrame ) {
        MLOG_ERROR("CAM<%d>, av_frame_alloc failed", m_cam_id);
        if(m_pFrame) av_frame_free(&m_pFrame);
        if(m_pFlushFrame) av_frame_free(&m_pFlushFrame); // 안전하게 해제
        avcodec_close(m_pContext);
        avcodec_free_context(&m_pContext);
        return FALSE;
    }

    m_nCodec  = AVCodecIDToDefineCodecID(codecId);
    m_bInited = TRUE;

    // -------------------------------------------------------------------------
    // 6. [최종 성공 로그] 한 줄로 요약 출력
    // -------------------------------------------------------------------------
    // SPS/PPS 정보가 너무 길다면 잘라내거나 핵심만 남기는 처리 필요
    // 예: sps_summary가 "Profile: 66, Level: 31..." 이면 그대로 씀

    // SPS/PPS 정보가 비어있지 않으면 포함하여 출력
    std::string extra_info = "";
    if( !pps_summary.empty() ) extra_info += " | " + pps_summary;
    if( !sps_summary.empty() ) extra_info += " | " + sps_summary;

    // 최종 로그: CAM<ID> Decoder Opened: [코덱] (해상도) [SPS/PPS 정보]
    MLOG_INFO("CAM<%d> Decoder Opened: %s (%s) | %-4dx%-4d | Extra: %3dB%s",
        m_cam_id,
        avcodec_get_name(m_pContext->codec_id),
        m_pCodec->name,
        m_pContext->width, m_pContext->height,
        m_pContext->extradata_size,
        extra_info.c_str()
    );

    return TRUE;
}

BOOL CVideoDecoder::init( InitConfig& config )
{
	return this->init(
		config.codec_id,
		config.v_width,
		config.v_height,
		config.p_extra_data,
		config.n_extra_size,
		config.gpu_id,
		config.cam_id
	);
}

CVideoDecoder::PacketSendAction CVideoDecoder::checkValidPacket( AVPacket* pkt )
{
	if( !m_bInited || !pkt || pkt->size == 0 )
		return CVideoDecoder::PacketSendAction::DROP;

	int         nal_unit_type          = -1;
	int         offset                 = -1;
	const char* nal_type_str           = "Unknown";
	uint8_t*    nal_data_ptr           = nullptr;
	int         nal_data_len           = 0;

	AVCodecID current_codec_id = m_pContext->codec_id;

	// H.264 / H.265 코덱이고, 최소한의 헤더 크기 이상일 때만 NAL 타입 검사
	if( (current_codec_id == AV_CODEC_ID_H264 || current_codec_id == AV_CODEC_ID_H265) && pkt->size > 4 )
	{
		// 시작 코드 찾기
		if( pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 0 && pkt->data[3] == 1 ){
			offset = 4;
		}
		else if( pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1 ){
			offset = 3;
		}

		// 시작 코드를 찾았고 NAL 헤더가 존재할 경우
		if( offset != -1 && pkt->size > offset )
		{
			nal_data_ptr = pkt->data + offset;
			nal_data_len = pkt->size - offset;

			// H.264 처리
			if( current_codec_id == AV_CODEC_ID_H264 )
			{
				nal_unit_type = nal_data_ptr[0] & 0x1F;

				if( nal_unit_type == H264_NAL_SPS )
				{
					nal_type_str = "SPS";

					// 새로운 SPS 파싱
					H264SPSInfo new_sps;
					new_sps.valid = parse_sps( nal_data_ptr, nal_data_len, new_sps );

					// 파싱에 성공했고, 기존 SPS와 내용이 다른지 비교
					if( new_sps.valid && new_sps != m_current_sps )
					{
						MLOG_INFO("CAM<%d> Received SPS (Size: %d bytes) Changed", m_cam_id, nal_data_len);
						MLOG_INFO("CAM<%d> New SPS Info: %s", m_cam_id, sps_info_to_string(new_sps).c_str());

						// 기존 SPS 정보 로깅 (선택적)
						if( m_current_sps.valid ){
							MLOG_INFO("CAM<%d> Old SPS Info: %s", m_cam_id, sps_info_to_string(m_current_sps).c_str());
						} else {
							MLOG_INFO("CAM<%d> Old SPS Info: N/A (or invalid)", m_cam_id);
						}

						// 현재 SPS 정보 업데이트
						m_current_sps = new_sps;

						// m_current_extradata 재조립 로직은 디코더 재초기화 방식이 아니므로 일단 보류
						// MLOG_INFO("CAM<%d> m_current_extradata updated with new SPS...", m_cam_id);

						return CVideoDecoder::PacketSendAction::SEND_SPS_CHANGED; // 변경된 SPS 전송 (오류 예상)
					}
					else
					{
						// 파싱 실패했거나, 기존과 동일한 SPS는 버림
						return CVideoDecoder::PacketSendAction::DROP;
					}
				}
				else if( nal_unit_type == H264_NAL_PPS )
				{
					nal_type_str = "PPS";

					// 새로운 PPS 파싱
					H264PPSInfo new_pps;
					new_pps.valid = parse_pps( nal_data_ptr, nal_data_len, new_pps );

					// 파싱에 성공했고, 기존 PPS와 내용이 다른지 비교
					if( new_pps.valid && new_pps != m_current_pps )
					{
						MLOG_INFO("CAM<%d> Received PPS (Size: %d bytes) Changed", m_cam_id, nal_data_len);
						MLOG_INFO("CAM<%d> New PPS Info: %s", m_cam_id, pps_info_to_string(new_pps).c_str());

						if( m_current_pps.valid ){
							MLOG_INFO("CAM<%d> Old PPS Info: %s", m_cam_id, pps_info_to_string(m_current_pps).c_str());
						} else {
							MLOG_INFO("CAM<%d> Old PPS Info: N/A (or invalid)", m_cam_id);
						}

						// 현재 PPS 정보 업데이트
						m_current_pps = new_pps;

						// m_current_extradata 재조립 로직 보류
						// MLOG_INFO("CAM<%d> m_current_extradata updated with new PPS...", m_cam_id);

						return CVideoDecoder::PacketSendAction::SEND_PPS_CHANGED; // 변경된 PPS 전송 (오류 예상)
					}
					else
					{
						// 파싱 실패했거나, 기존과 동일한 PPS는 버림
						return CVideoDecoder::PacketSendAction::DROP;
					}
				}
				else if( nal_unit_type == H264_NAL_AUD || nal_unit_type == H264_NAL_SEI || nal_unit_type == H264_NAL_FILLER_DATA )
				{
					nal_type_str = (nal_unit_type == H264_NAL_AUD) ? "AUD" : (nal_unit_type == H264_NAL_SEI) ? "SEI" : "FillerData";
					MLOG_TRACE("CAM<%d> Dropping ignorable NAL unit (Type: %d [%s], Size: %d bytes)", m_cam_id, nal_unit_type, nal_type_str, pkt->size);
					return CVideoDecoder::PacketSendAction::DROP;
				}
				else if( nal_unit_type == H264_NAL_SLICE || nal_unit_type == H264_NAL_IDR )
				{
					// SPS 또는 PPS가 아직 유효하지 않으면 (초기화 실패 또는 아직 못 받음)
					if( !m_current_sps.valid || !m_current_pps.valid )
					{
						MLOG_TRACE("CAM<%d> Dropping video slice (Type: %d) because SPS/PPS are not valid yet.", m_cam_id, nal_unit_type);
						return CVideoDecoder::PacketSendAction::DROP;
					}

					// SPS/PPS가 유효하면 전송
					nal_type_str = (nal_unit_type == H264_NAL_SLICE) ? "P-Slice" : "IDR-Slice";
					return CVideoDecoder::PacketSendAction::SEND_NORMAL;
				}
				// 그 외 타입 (2, 3, 4 등) h264.h 참조
				else
				{
					nal_type_str = "Other NAL";
					MLOG_TRACE("CAM<%d> Passing other NAL unit (Type: %d [%s], Size: %d bytes)", m_cam_id, nal_unit_type, nal_type_str, pkt->size);
					return CVideoDecoder::PacketSendAction::SEND_NORMAL; // 일단 보내봄
				}
			}
			// H.265 (HEVC)
			else
			{
				nal_unit_type = (nal_data_ptr[0] >> 1) & 0x3F;

				// VPS(32), SPS(33), PPS(34), AUD(35), SEI(39, 40), Filler(46)
				if( nal_unit_type == HEVC_NAL_VPS ||
					nal_unit_type == HEVC_NAL_SPS ||
					nal_unit_type == HEVC_NAL_PPS ||
					nal_unit_type == HEVC_NAL_AUD ||
					nal_unit_type == HEVC_NAL_SEI_PREFIX ||
					nal_unit_type == HEVC_NAL_SEI_SUFFIX ||
					nal_unit_type == 46 /*Filler*/ )
				{
					// ... (H.265 파싱 및 비교 로직 구현 필요) ...

					if     ( nal_unit_type == HEVC_NAL_VPS ) nal_type_str = "VPS";
					else if( nal_unit_type == HEVC_NAL_SPS ) nal_type_str = "SPS";
					else if( nal_unit_type == HEVC_NAL_PPS ) nal_type_str = "PPS";
					else if( nal_unit_type == HEVC_NAL_AUD ) nal_type_str = "AUD";
					else if( nal_unit_type == HEVC_NAL_SEI_PREFIX || nal_unit_type == HEVC_NAL_SEI_SUFFIX ) nal_type_str = "SEI";
					else nal_type_str = "FillerData";

					//MLOG_TRACE("CAM<%d> Dropping HEVC Parameter/Ignorable Set (Type: %d [%s], Size: %d bytes).", m_cam_id, nal_unit_type, nal_type_str, nal_data_len);
					//return CVideoDecoder::PacketSendAction::DROP;
					return CVideoDecoder::PacketSendAction::SEND_NORMAL; // 드랍해야하나?
				}
				// 비디오 슬라이스 (0~31)
				else if (nal_unit_type <= 31)
				{
					nal_type_str = "HEVC Video Slice";
					return CVideoDecoder::PacketSendAction::SEND_NORMAL;
				}
				// 그 외 타입
				else
				{
					nal_type_str = "Other HEVC NAL";
					MLOG_TRACE("CAM<%d> Passing other HEVC NAL unit (Type: %d [%s], Size: %d bytes)", m_cam_id, nal_unit_type, nal_type_str, pkt->size);
					return CVideoDecoder::PacketSendAction::SEND_NORMAL; // 일단 보내봄
				}
			}
		}
		// 시작 코드를 못 찾거나 NAL 헤더 없음
		else
		{
			MLOG_WARN("CAM<%d> Dropping packet with invalid/incomplete NAL header (Size: %d bytes)", m_cam_id, pkt->size);
			return CVideoDecoder::PacketSendAction::DROP;
		}
	}
	// MJPEG 코덱
	else if (current_codec_id == AV_CODEC_ID_MJPEG)
	{
		nal_type_str = "JPEG Frame";
		return CVideoDecoder::PacketSendAction::SEND_NORMAL;
	}
	// 그 외 코덱 또는 짧은 패킷
	else if (pkt->size > 0)
	{
		// 시작 코드조차 되기 힘든 크기
		if (pkt->size <= 4)
		{
			MLOG_WARN("CAM<%d> Dropping extremely short packet (Size: %d bytes)", m_cam_id, pkt->size);
			return CVideoDecoder::PacketSendAction::DROP;
		}
		// 알 수 없는 코덱
		else
		{
			MLOG_WARN("CAM<%d> Passing packet for unknown/unfiltered codec type %d (Size: %d bytes)", m_cam_id, current_codec_id, pkt->size);
			return CVideoDecoder::PacketSendAction::SEND_NORMAL;
		}
	}

	// size 0 (함수 시작 부분에서 이미 처리됨)
	return CVideoDecoder::PacketSendAction::DROP;
}

BOOL CVideoDecoder::decode( AVPacket * pkt )
{
	if( !m_bInited )
		return FALSE;

	// checkValidPacket 함수를 먼저 호출하여 패킷 유효성 검사 및 필터링
	PacketSendAction send_action = checkValidPacket(pkt);

	// 버려야 하는 패킷 (SPS/PPS 동일, AUD, SEI 등)
	if( send_action == PacketSendAction::DROP )
	{
		return TRUE; // 오류는 아니므로 TRUE 반환
	}

	// MLOG_DEBUG("CAM<%d> Sending packet to decoder (Action: %d, Size: %d bytes)",
	// 			m_cam_id, static_cast<int>(send_action), pkt ? pkt->size : 0);

	int ret = avcodec_send_packet( m_pContext, pkt ); // 한 20% 디코드가 먹고 있음
	if( ret < 0 )
	{
		// 변경된 SPS/PPS를 보냈고, 예상된 'Invalid data' 오류가 발생한 경우
		if( ( send_action == PacketSendAction::SEND_SPS_CHANGED || send_action == PacketSendAction::SEND_PPS_CHANGED ) && ret == AVERROR_INVALIDDATA )
		{
			// MLOG_WARN("CAM<%d> Sent changed SPS/PPS, received expected AVERROR_INVALIDDATA. Assuming context updated internally. (Size: %d)",
			//		   m_cam_id, pkt ? pkt->size : 0);

			// 오류로 간주하지 않고, 내부 업데이트가 발생했을 것으로 가정
			// receive_frame 루프를 계속 진행
		}
		// 디코더 입력 큐가 꽉 찬 경우
		else if( ret == AVERROR(EAGAIN) )
		{
			MLOG_WARN("CAM<%d> Decoder input queue full (EAGAIN). Dropping current packet.", m_cam_id);
			// EAGAIN 시에도 receive_frame 루프를 실행해야 함 (큐 비우기)
		}
		// 그 외 실제 오류
		else
		{
			// char errbuf[AV_ERROR_MAX_STRING_SIZE];
			// av_strerror(ret, errbuf, sizeof(errbuf));
			// MLOG_ERROR("CAM<%d>, error sending packet (Size: %d bytes) => %s (%d)", m_cam_id, pkt ? pkt->size : 0, errbuf, ret );
			// 특정 경우에서 너무 많이뜸...
			// MLOG_DEBUG("CAM<%d>, error sending packet (Size: %d bytes) => %s (%d)", m_cam_id, pkt ? pkt->size : 0, errbuf, ret );
			return FALSE; // 실제 오류 발생 시 디코딩 실패
		}
	}

	while( true )
	{
		ret = avcodec_receive_frame( m_pContext, m_pTryDecodeFrame ); // m_pTryDecodeFrame->format : AV_PIX_FMT_YUV420P

		// 더 이상 나올 프레임이 없거나, 더 많은 입력이 필요한 경우
		if( ret == AVERROR(EAGAIN) || ret == AVERROR_EOF )
		{
			break;
		}
		// 실제 오류 발생
		else if( ret < 0 )
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			MLOG_ERROR("CAM<%d>, error receiving frame => %s (%d)", m_cam_id, errbuf, ret);
			return FALSE;
		}

		if( m_pTryDecodeFrame->flags & AV_FRAME_FLAG_CORRUPT ){
        	MLOG_WARN("CAM<%d> Decoded frame is incomplete or has errors", m_cam_id);
    	}

		if( m_pTryDecodeFrame->decode_error_flags > 0 ){
			MLOG_WARN("CAM<%d> Decode Error Flags detected: %d", m_cam_id, m_pTryDecodeFrame->decode_error_flags);
		}

		// Total 45% 가량의 CPU 사용량 중 20%+ 를 해당 디텍트에서 사용 중임
		// 아래 함수 주석 시 영상 스트림만 되고 디텍트 비활성화됨
		if( m_pCallback )
		{
			AVFrame* pOutFrame = av_frame_alloc();
			if( pOutFrame == nullptr )
				continue;

			av_frame_move_ref( pOutFrame, m_pTryDecodeFrame ); // 참조 이동

			m_pCallback( pOutFrame, m_pUserdata );
			// -> rtsp_proxy_vdecoder_cb(AVFrame * frame, void * pUserData);
			//     ->  CRtspProxy::detectCallback(AVFrame* frame, int type);
		}
	}

	return TRUE;
}

// FROM : CRtspProxy::videoDataDecode(uint8* p_data, int len)
BOOL CVideoDecoder::decode( uint8 * data, int len )
{
	// 1. AVPacket을 스택이 아닌 힙(heap)에 동적으로 할당합니다.
	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		// 메모리 할당 실패 시 에러 처리
		log_print(LOG_ERR, "%s, av_packet_alloc failed", __FUNCTION__);
		return FALSE;
	}
	// av_init_packet()은 av_packet_alloc()이 내부적으로 처리하므로 더 이상 필요 없습니다.

	// 2. 전달받은 데이터의 참조를 packet에 설정합니다.
	//    (주의: 이 packet이 처리되는 동안 'data' 포인터는 반드시 유효해야 합니다.)
	packet->data = data;
	packet->size = len;

	// 3. 동적으로 할당된 packet을 다른 decode() 함수로 전달합니다.
	BOOL result = decode(packet);

	// 4. decode() 함수 내부의 avcodec_send_packet()이 packet 데이터의 참조를 가져갔으므로,
	//    여기서는 AVPacket 래퍼(wrapper) 객체만 안전하게 해제합니다.
	av_packet_free(&packet);

	return result;
}

int CVideoDecoder::render()
{
	if (m_pCallback)
		m_pCallback(m_pFrame, m_pUserdata);

	return 1;
}

void CVideoDecoder::flush()
{
	if( NULL == m_pContext || NULL == m_pContext->codec || !(m_pContext->codec->capabilities | AV_CODEC_CAP_DELAY) )
	{
		return;
	}

	// decode(NULL); // 플러시를 위해 NULL 패킷 전송 로직 필요

	// 명시적인 플러시 호출 (NULL 패킷 대신)
	if( m_bInited && m_pContext )
	{
		int ret = avcodec_send_packet(m_pContext, NULL); // NULL 패킷 전송
		if( ret < 0 && ret != AVERROR_EOF ){
			MLOG_WARN("CAM<%d> Failed to send flush packet (NULL): %d", m_cam_id, ret);
		}

		// 큐에 남아있는 프레임 모두 뽑아내기
		while( ret != AVERROR_EOF )
		{
			ret = avcodec_receive_frame(m_pContext, m_pTryDecodeFrame);
			if( ret == AVERROR_EOF || ret == AVERROR(EAGAIN) )
			{
				break; // 플러시 완료 또는 데이터 없음
			}
			else if( ret < 0 )
			{
				MLOG_ERROR("CAM<%d> Error receiving flushed frame: %d", m_cam_id, ret);
				break;
			}

			if( m_pCallback != nullptr )
            {
                AVFrame* pOutFrame = av_frame_alloc();
                if( pOutFrame != nullptr )
                {
                    av_frame_move_ref( pOutFrame, m_pTryDecodeFrame );
                    m_pCallback( pOutFrame, m_pUserdata );
                }
            }
		}
	}
}

int CVideoDecoder::getWidth() const
{
	// [수정] 파싱된 SPS 정보가 있으면 우선 반환
	if (m_current_sps.valid && m_current_sps.width > 0)
	{
		return m_current_sps.width;
	}
	// 없으면 m_pContext 값 반환 (초기화 시 설정된 값)
	if (m_pContext)
	{
		return m_pContext->width;
	}
	return m_nVideoWidth; // 최후의 보루
}

int CVideoDecoder::getHeight() const
{
	// [수정] 파싱된 SPS 정보가 있으면 우선 반환
	if (m_current_sps.valid && m_current_sps.height > 0)
	{
		return m_current_sps.height;
	}
	// 없으면 m_pContext 값 반환
	if (m_pContext)
	{
		return m_pContext->height;
	}
	return m_nVideoHeight; // 최후의 보루
}

