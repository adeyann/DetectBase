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

#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include "sys_inc.h"
#include "media_format.h"
#include "h264_parser.h"

extern "C"
{
	#include <libavcodec/avcodec.h>
	#include <libavutil/avutil.h>
	#include <libavutil/frame.h>
	#include <libavutil/opt.h>
	#include <libavutil/error.h>
//	#include "libswscale/swscale.h"   // 디코딩만 하면 불필요할 수 있음
//	#include "libavformat/avformat.h" // 디코딩만 하면 불필요할 수 있음
}

typedef void (*VDCB)(AVFrame * frame, void *pUserdata);

class CVideoDecoder
{
public:
	CVideoDecoder();
	virtual ~CVideoDecoder();

public:
	struct InitConfig
	{
		AVCodecID codec_id     = AV_CODEC_ID_NONE;
		int       v_width      = 0;
		int       v_height     = 0;
		uint8_t*  p_extra_data = nullptr;
		int       n_extra_size = 0;
		int       gpu_id       = -1;
		int       cam_id       = -1;
	};

public:
	// 초기화 함수
	BOOL init( int codec, int gpuId ); // 구 버전 호환용 (사용 비권장)
	BOOL init( AVCodecID codecId, int width, int height, uint8_t* pExtraData, int nExtraDataSize, int gpuId, int camId = -1 );
	BOOL init( InitConfig& config );


	// 디코딩 함수
	BOOL decode( uint8 * data, int len ); // 구 버전 호환용 (사용 비권장)
	BOOL decode( AVPacket *pkt );

	// 콜백 설정
	void setCallback( VDCB pCallback, void * pUserdata )
	{
		m_pCallback = pCallback;
		m_pUserdata = pUserdata;
	}

	// 해제 함수
	void uninit();

	int getWidth()  const;
	int getHeight() const;

private:
	/*
		 - 패킷 처리 액션 정의 (checkValidPacket 반환 타입)
		 - SPS/PPS/AUD/SEI 등은 DROP, 비디오 슬라이스는 SEND_NORMAL
		 - SPS/PPS 변경 시에는 SEND_SPS_CHANGED / SEND_PPS_CHANGED 반환
		 - 콜백 함수에서 AVERROR_INVALIDDATA 처리 필요
		 - (참고: RTSP/RTMP 프로토콜에서 SPS/PPS 변경 시 재전송 필요)
		 - 예: SPS 변경 시 AVPacket에 SPS 포함 후 SEND_SPS_CHANGED 반환
		 - 디코더 내부에서만 사용할 열거형
	*/
	enum class PacketSendAction
	{
		DROP,             // 패킷 버림 (SPS/PPS 동일, AUD, SEI 등)
		SEND_NORMAL,      // 정상적으로 전송 (비디오 슬라이스 등)
		SEND_SPS_CHANGED, // 변경된 SPS 전송 (AVERROR_INVALIDDATA 예상)
		SEND_PPS_CHANGED  // 변경된 PPS 전송 (AVERROR_INVALIDDATA 예상)
	};

private:
	int  render();
    void flush();

	// NAL 유닛 검사 및 비교 수행
	PacketSendAction checkValidPacket( AVPacket* pkt );
	bool setDecoderInitContext( void );

private:
	BOOL			m_bInited      = FALSE;
    int		        m_nCodec       = VIDEO_CODEC_NONE;
	int				m_nVideoWidth  = 0;
	int				m_nVideoHeight = 0;
	int             m_cam_id       = -1;

	const AVCodec*  m_pCodec          = nullptr;
	AVCodecContext* m_pContext        = nullptr;
	AVFrame *		m_pFrame          = nullptr;
	AVFrame*		m_pFlushFrame     = nullptr;
	AVFrame*		m_pTryDecodeFrame = nullptr;

	VDCB            m_pCallback       = nullptr;
    void*           m_pUserdata       = nullptr;
	bool			delPFrame         = true; // m_pFrame 소유권 플래그 (역할 불분명)

	H264SPSInfo m_current_sps; // 파싱된 현재 SPS 정보 저장
	H264PPSInfo m_current_pps; // 파싱된 현재 PPS 정보 저장
	// HEVC 용 구조체도 필요 시 추가
};
#endif
