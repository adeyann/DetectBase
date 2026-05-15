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
#include "rtp_rx.h"



BOOL rtp_data_rx(RTPRXI * p_rxi, uint8 * p_data, int len)
{
	uint8 * p_rtp = p_data;
	uint32  rtp_len = len;

	// Check for the 12-byte RTP header:
    if (rtp_len < 12)
    {
    	return FALSE;
    }

    uint32 rtpHdr = ntohl(*(uint32*)p_rtp); p_rtp += 4; rtp_len -= 4;
    BOOL   rtpMarkerBit = (rtpHdr & 0x00800000) != 0;
    uint32 rtpSeq = rtpHdr & 0xFFFF;
    uint32 rtpTimestamp = ntohl(*(uint32*)(p_rtp)); p_rtp += 4; rtp_len -= 4;
    uint32 rtpSSRC = ntohl(*(uint32*)(p_rtp)); p_rtp += 4; rtp_len -= 4;

	// Check the RTP version number (it should be 2)
    if ((rtpHdr & 0xC0000000) != 0x80000000)
    {
    	return FALSE;
    }

    // Skip over any CSRC identifiers in the header
    uint32 cc = (rtpHdr >> 24) & 0x0F;
    if (rtp_len < cc * 4)
    {
    	return FALSE;
    }
    p_rtp += cc * 4; rtp_len -= cc * 4;

    // Check for (& ignore) any RTP header extension
    if (rtpHdr & 0x10000000)
    {
		if (rtp_len < 4)
		{
			return FALSE;
		}

		uint32 extHdr = ntohl(*(uint32*)(p_rtp)); p_rtp += 4; rtp_len -= 4;
		uint32 remExtSize = 4 * (extHdr & 0xFFFF);
		if (rtp_len < remExtSize)
		{
			return FALSE;
		}
		p_rtp += remExtSize; rtp_len -= remExtSize;
    }

    // Discard any padding bytes:
    if (rtpHdr & 0x20000000)
    {
		if (rtp_len == 0)
		{
			return FALSE;
		}

		uint32 numPaddingBytes = (uint32)p_rtp[rtp_len-1];
		if (rtp_len < numPaddingBytes)
		{
			return FALSE;
		}
		rtp_len -= numPaddingBytes;
    }

    if (p_rxi->ssrc && p_rxi->ssrc != rtpSSRC)
	{
		log_print(LOG_ERR, "%s, p_rxi->ssrc[%u] != rtp ssrc[%u]!!!\r\n", __FUNCTION__, p_rxi->ssrc, rtpSSRC);
	}
	p_rxi->ssrc = rtpSSRC;

	if (p_rxi->prev_seq && p_rxi->prev_seq != rtpSeq - 1)
	{
		p_rxi->rxf_loss = 1;
		log_print(H_LOG_WARN, "%s, mid seq[%u]lost, cur seq[%u]!!!\r\n", __FUNCTION__, p_rxi->prev_seq+1, rtpSeq);
	}
	p_rxi->prev_seq = rtpSeq;

	p_rxi->rxf_marker = rtpMarkerBit;
	p_rxi->prev_ts = rtpTimestamp;

	p_rxi->p_data = p_rtp;
	p_rxi->len = rtp_len;

    return TRUE;
}




