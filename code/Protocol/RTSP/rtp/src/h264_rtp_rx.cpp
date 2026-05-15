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
#include "rtp.h"
#include "h264_rtp_rx.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/avcodec.h"

/***********************************************************************/

BOOL h264_data_rx(H264RXI * p_rxi, uint8 * p_data, int len)
{
	uint8 * headerStart = p_data;
	uint32  packetSize = len;
	uint32  numBytesToSkip;
	uint8 	fCurPacketNALUnitType;
	BOOL 	fCurrentPacketBeginsFrame = FALSE;
	BOOL 	fCurrentPacketCompletesFrame = FALSE;

	// Check the 'nal_unit_type' for special 'aggregation' or 'fragmentation' packets
	if (packetSize < 1)
	{
		return FALSE;
	}

	fCurPacketNALUnitType = (headerStart[0] & 0x1F);
	switch (fCurPacketNALUnitType)
	{
	case 24:  					// STAP-A
	{
		//const uint8_t start_sequence[] = { 0, 0, 0, 1 };

		//Skip NAL header byte
		headerStart++;
		len--;

		while (len > 2)
		{
			//First two bytes contain the size of the NAL unit
			uint16_t nalSize = AV_RB16(headerStart);
			headerStart += 2;
			len -= 2;
			if (nalSize <= len)
			{
				//Remaining 5 bits of the first byte contain the NAL unit type
				uint8 nalUnitType = (headerStart[0] & 0x1F);
				//printf("NAL1 unit type is %d and size is %d\n", (int)nalUnitType, (int)nalSize);

				//Copy only the NAL unit data of the current packet
				uint8_t* dst = (uint8_t*)malloc(nalSize);
				memcpy(dst, headerStart, nalSize);

				//Check if NAL unit type is SPS or PPS
				if (nalUnitType == H264_NAL_SPS && p_rxi->param_sets.sps_len == 0)
				{
					h264_save_sps(p_rxi, dst, nalSize);
				}
				else if (nalUnitType == H264_NAL_PPS && p_rxi->param_sets.pps_len == 0)
				{
					h264_save_pps(p_rxi, dst, nalSize);
				}

				//Pass data to video callback
				p_rxi->d_offset = 0;
				memcpy(p_rxi->p_buf + 4, dst, nalSize);
				p_rxi->d_offset += nalSize;
				p_rxi->p_buf[0] = 0;
				p_rxi->p_buf[1] = 0;
				p_rxi->p_buf[2] = 0;
				p_rxi->p_buf[3] = 1;

				if (p_rxi->pkt_func)
				{
					p_rxi->pkt_func(p_rxi->p_buf, p_rxi->d_offset + 4, p_rxi->rtprxi.prev_ts, p_rxi->rtprxi.prev_seq, p_rxi->user_data);
				}
				free(dst);
				p_rxi->d_offset = 0;

				headerStart += nalSize;
				len -= nalSize;
			}
			else
			{
				printf("NAL unit in RTP STAP-A packet size is %d, but the remaining bytes in packet is only %d\n", (int)nalSize, (int)len);
			}
		}
		return TRUE;
		//numBytesToSkip = 1; 	// discard the type byte
		//break;
	}

	case 25: case 26: case 27:  // STAP-B, MTAP16, or MTAP24
		numBytesToSkip = 3; 	// discard the type byte, and the initial DON
		break;

	case 28: case 29: 			// FU-A or FU-B
	{
	    // For these NALUs, the first two bytes are the FU indicator and the FU header.
	    // If the start bit is set, we reconstruct the original NAL header into byte 1

	    if (packetSize < 2)
	    {
	    	return FALSE;
	    }

	    uint8 startBit = headerStart[1] & 0x80;
	    uint8 endBit = headerStart[1] & 0x40;
	    if (startBit)
	    {
			fCurrentPacketBeginsFrame = TRUE;

			headerStart[1] = (headerStart[0] & 0xE0) | (headerStart[1] & 0x1F);
			numBytesToSkip = 1;
	    }
	    else
	    {
			// The start bit is not set, so we skip both the FU indicator and header:
			fCurrentPacketBeginsFrame = FALSE;
			numBytesToSkip = 2;
	    }

		fCurrentPacketCompletesFrame = (endBit != 0);
		break;
	}

	default:
		// This packet contains one complete NAL unit:
		fCurrentPacketBeginsFrame = fCurrentPacketCompletesFrame = TRUE;
		numBytesToSkip = 0;
		break;
	}

	// save the H264 parameter sets

	if (fCurPacketNALUnitType == H264_NAL_SPS && p_rxi->param_sets.sps_len == 0)
	{
		h264_save_sps(p_rxi, p_data, len);
	}
	else if (fCurPacketNALUnitType == H264_NAL_PPS && p_rxi->param_sets.pps_len == 0)
	{
		h264_save_pps(p_rxi, p_data, len);
	}

	if (fCurrentPacketBeginsFrame)
	{
		p_rxi->d_offset = 0;
	}

  	if ((p_rxi->d_offset + 4 + packetSize - numBytesToSkip) >= RTP_MAX_VIDEO_BUFF)
	{
		log_print(LOG_ERR, "%s, fragment packet too big %d!!!", __FUNCTION__, p_rxi->d_offset + 4 + packetSize - numBytesToSkip);
		return FALSE;
	}

	memcpy(p_rxi->p_buf + p_rxi->d_offset + 4, headerStart + numBytesToSkip, packetSize - numBytesToSkip);
	p_rxi->d_offset += packetSize - numBytesToSkip;

	if (fCurrentPacketCompletesFrame)
	{
		p_rxi->p_buf[0] = 0;
		p_rxi->p_buf[1] = 0;
		p_rxi->p_buf[2] = 0;
		p_rxi->p_buf[3] = 1;

		// --- [중요] 꼬리 패딩 0 보장 ---
		int payload_len = p_rxi->d_offset + 4;
		// p_rxi->buf_len == usable payload length 이므로, padding은 p_rxi->p_buf + p_rxi->buf_len 뒤에 위치
		// 다만, 디코더는 "실제 payload_len 바로 뒤"에 패딩이 0이라고 가정하므로 다음을 보장합니다.
		// (32바이트 memset이라 비용 매우 작습니다)
		memset(p_rxi->p_buf + payload_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

		if (p_rxi->pkt_func)
  		{
			// video_data_cb
			// 여기 들어가기 전에는 45%+ 중에서 3% 가량밖에 안씀
			// video_data_cb :: p_rxi->p_buf = raw data packet
			p_rxi->pkt_func(p_rxi->p_buf, p_rxi->d_offset + 4, p_rxi->rtprxi.prev_ts, p_rxi->rtprxi.prev_seq, p_rxi->user_data);
  		}

		p_rxi->d_offset = 0;
	}

	return TRUE;
}

BOOL h264_rtp_rx(H264RXI * p_rxi, uint8 * p_data, int len)
{
	if (p_rxi == NULL)
	{
		return FALSE;
	}

	if (!rtp_data_rx(&p_rxi->rtprxi, p_data, len))
	{
		printf("Parsing rtp header data returned false\n");
		return FALSE;
	}

	return h264_data_rx(p_rxi, p_rxi->rtprxi.p_data, p_rxi->rtprxi.len);
}

BOOL h264_rxi_init(H264RXI *p_rxi, VRTPRXCBF cbf, void *p_userdata)
{
    memset(p_rxi, 0, sizeof(H264RXI));

    // 유효 페이로드(조립 가능한 최대 길이)
    const int usable_len = RTP_MAX_VIDEO_BUFF;

    // 총 할당: [선행여유 32] + [유효 페이로드] + [꼬리패딩]
    const int alloc_len = 32 + usable_len + AV_INPUT_BUFFER_PADDING_SIZE;

    // 전체 0 초기화로 할당(패딩 0 보장에 유리)
    p_rxi->p_buf_org = (uint8 *)calloc(1, alloc_len);
    if (!p_rxi->p_buf_org) {
        return -1; // 기존 코드 스타일 유지
    }

    // 기존과 동일하게 선행 32바이트 이동(정렬/스타트코드 등 용도)
    p_rxi->p_buf     = p_rxi->p_buf_org + 32;
    p_rxi->buf_len   = usable_len;               // "유효 데이터로 쓸 수 있는 길이"
    p_rxi->pkt_func  = cbf;
    p_rxi->user_data = p_userdata;

    // (선택) 정렬 보장이 필요하면 여기서 정렬 assert 가능
    // assert(((uintptr_t)p_rxi->p_buf % 16) == 0);

    return 0;
}

void h264_rxi_deinit(H264RXI * p_rxi)
{
	if (p_rxi->p_buf_org)
	{
		free(p_rxi->p_buf_org);
    }

	memset(p_rxi, 0, sizeof(H264RXI));
}

void h264_save_sps(H264RXI* p_rxi, uint8* p_data, int len)
{
	if (len <= static_cast<int>(sizeof(p_rxi->param_sets.sps)) - 4)
	{
		int offset = 0;

		if (p_data[0] == 0 && p_data[1] == 0 && p_data[2] == 0 && p_data[3] == 1)
		{
		}
		else
		{
			p_rxi->param_sets.sps[0] = 0;
			p_rxi->param_sets.sps[1] = 0;
			p_rxi->param_sets.sps[2] = 0;
			p_rxi->param_sets.sps[3] = 1;

			offset = 4;
		}

		memcpy(p_rxi->param_sets.sps + offset, p_data, len);
		p_rxi->param_sets.sps_len = len + offset;
	}
}

void h264_save_pps(H264RXI* p_rxi, uint8* p_data, int len)
{
	if (len <= static_cast<int>(sizeof(p_rxi->param_sets.pps)) - 4)
	{
		int offset = 0;

		if (p_data[0] == 0 && p_data[1] == 0 && p_data[2] == 0 && p_data[3] == 1)
		{
		}
		else
		{
			p_rxi->param_sets.pps[0] = 0;
			p_rxi->param_sets.pps[1] = 0;
			p_rxi->param_sets.pps[2] = 0;
			p_rxi->param_sets.pps[3] = 1;

			offset = 4;
		}

		memcpy(p_rxi->param_sets.pps + offset, p_data, len);
		p_rxi->param_sets.pps_len = len + offset;
	}
}



