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
#include "h265_rtp_rx.h"


/****************************************************************************/

BOOL h265_data_rx(H265RXI * p_rxi, uint8 * p_data, int len)
{
	uint8* headerStart = p_data;
	uint32 packetSize = len;
	uint16 DONL = 0;
	uint32 numBytesToSkip;
	uint8  fCurPacketNALUnitType;
	BOOL   fCurrentPacketBeginsFrame = FALSE;
	BOOL   fCurrentPacketCompletesFrame = FALSE;

	// Check the Payload Header's 'nal_unit_type' for special aggregation or fragmentation packets:
	if (packetSize < 2)
	{
		return FALSE;
	}

	fCurPacketNALUnitType = (headerStart[0] & 0x7E) >> 1;

	switch (fCurPacketNALUnitType)
	{
	case 48:
	{
		// Aggregation Packet (AP)
		// We skip over the 2-byte Payload Header, and the DONL header (if any).
		if (p_rxi->rxf_don)
		{
			if (packetSize < 4)
			{
				return FALSE;
			}

			DONL = (headerStart[2] << 8) | headerStart[3];
			numBytesToSkip = 4;
		}
		else
		{
			numBytesToSkip = 2;
		}
		break;
	}

	case 49:
	{
		// Fragmentation Unit (FU)
		// This NALU begins with the 2-byte Payload Header, the 1-byte FU header, and (optionally)
		// the 2-byte DONL header.
		// If the start bit is set, we reconstruct the original NAL header at the end of these
		// 3 (or 5) bytes, and skip over the first 1 (or 3) bytes.

		if (packetSize < 3)
		{
			return FALSE;
		}

		uint8 startBit = headerStart[2] & 0x80; // from the FU header
		uint8 endBit = headerStart[2] & 0x40; 	// from the FU header
		if (startBit)
		{
			fCurrentPacketBeginsFrame = TRUE;

			uint8 nal_unit_type = headerStart[2] & 0x3F; // the last 6 bits of the FU header
			uint8 newNALHeader[2];

			newNALHeader[0] = (headerStart[0] & 0x81) | (nal_unit_type << 1);
			newNALHeader[1] = headerStart[1];

			if (p_rxi->rxf_don)
			{
				if (packetSize < 5)
				{
					return FALSE;
				}

				DONL = (headerStart[3] << 8) | headerStart[4];
				headerStart[3] = newNALHeader[0];
				headerStart[4] = newNALHeader[1];
				numBytesToSkip = 3;
			}
			else
			{
				headerStart[1] = newNALHeader[0];
				headerStart[2] = newNALHeader[1];
				numBytesToSkip = 1;
			}
		}
		else
		{
			// The start bit is not set, so we skip over all headers:
			fCurrentPacketBeginsFrame = FALSE;

			if (p_rxi->rxf_don)
			{
				if (packetSize < 5)
				{
					return FALSE;
				}

				DONL = (headerStart[3] << 8) | headerStart[4];
				numBytesToSkip = 5;
			}
			else
			{
				numBytesToSkip = 3;
			}
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

	// save the H265 parameter sets

	if (fCurPacketNALUnitType == HEVC_NAL_VPS && p_rxi->param_sets.vps_len == 0)
	{
		if (len <= static_cast<int>(sizeof(p_rxi->param_sets.vps)) - 4)
		{
			int offset = 0;

			if (p_data[0] == 0 && p_data[1] == 0 && p_data[2] == 0 && p_data[3] == 1)
			{
			}
			else
			{
				p_rxi->param_sets.vps[0] = 0;
				p_rxi->param_sets.vps[1] = 0;
				p_rxi->param_sets.vps[2] = 0;
				p_rxi->param_sets.vps[3] = 1;

				offset = 4;
			}

			memcpy(p_rxi->param_sets.vps+offset, p_data, len);
			p_rxi->param_sets.vps_len = len+offset;
		}
	}
	else if (fCurPacketNALUnitType == HEVC_NAL_SPS && p_rxi->param_sets.sps_len == 0)
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

			memcpy(p_rxi->param_sets.sps+offset, p_data, len);
			p_rxi->param_sets.sps_len = len+offset;
		}
	}
	else if (fCurPacketNALUnitType == HEVC_NAL_PPS && p_rxi->param_sets.pps_len == 0)
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

			memcpy(p_rxi->param_sets.pps+offset, p_data, len);
			p_rxi->param_sets.pps_len = len+offset;
		}
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

		if (p_rxi->pkt_func)
  		{
  			p_rxi->pkt_func(p_rxi->p_buf, p_rxi->d_offset + 4, p_rxi->rtprxi.prev_ts, p_rxi->rtprxi.prev_seq, p_rxi->user_data);
  		}

		p_rxi->d_offset = 0;
	}

	UNUSED( DONL );
	return TRUE;
}

BOOL h265_rtp_rx(H265RXI * p_rxi, uint8 * p_data, int len)
{
	if (p_rxi == NULL)
	{
		return FALSE;
	}

	if (!rtp_data_rx(&p_rxi->rtprxi, p_data, len))
	{
		return FALSE;
	}

	return h265_data_rx(p_rxi, p_rxi->rtprxi.p_data, p_rxi->rtprxi.len);
}

BOOL h265_rxi_init(H265RXI * p_rxi, VRTPRXCBF cbf, void * p_userdata)
{
	memset(p_rxi, 0, sizeof(H265RXI));

	p_rxi->buf_len = RTP_MAX_VIDEO_BUFF;

	p_rxi->p_buf_org = (uint8 *)malloc(p_rxi->buf_len);
	if (p_rxi->p_buf_org == NULL)
	{
		return -1;
	}

	p_rxi->p_buf = p_rxi->p_buf_org + 32;
	p_rxi->buf_len -= 32;
	p_rxi->pkt_func = cbf;
	p_rxi->user_data = p_userdata;

	return 0;
}

void h265_rxi_deinit(H265RXI * p_rxi)
{
	if (p_rxi->p_buf_org)
	{
		free(p_rxi->p_buf_org);
	}

	memset(p_rxi, 0, sizeof(H265RXI));
}




