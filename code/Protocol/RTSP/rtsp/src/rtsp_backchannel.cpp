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
#include "rtsp_backchannel.h"
#include "rtsp_rcua.h"
#include "rtp.h"
#include "rtsp_util.h"

#ifdef BACKCHANNEL

/***************************************************************************************/

/**
 * Send rtp data by TCP socket
 *
 * @param p_rua rtsp user agent
 * @param p_data rtp data
 * @param len rtp data len
 * @return -1 on error, or the data length has been sent
 */
int rtp_tcp_tx(RCUA * p_rua, void * p_data, int len)
{
	int offset = 0;

	if (p_rua->fd <= 0)
	{
	    return -1;
	}
	
	while (offset < len)
	{
		int tlen = send(p_rua->fd, (const char *)p_data+offset, len-offset, 0);
		if (tlen == (len-offset))
		{
			offset += tlen;
		}
		else
		{
			log_print(LOG_ERR, "%s, send failed, fd[%d],tlen[%d,%d],err[%d][%s]!!!\r\n",
				__FUNCTION__, p_rua->fd, tlen, (len-offset), errno, strerror(errno));			
			return -1;
		}
	}

	return offset;
}


int rtp_udp_tx(RCUA * p_rua, int av_t, char * p_rtp_data, int len)
{
	int slen;
	SOCKET fd = 0;
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(p_rua->ripstr);
	
	if (AV_TYPE_BACKCHANNEL == av_t)
	{	    
	    addr.sin_port = htons(p_rua->r_bc_port);
	    fd = p_rua->bc_udp_fd;
	    
	    if (p_rua->rtp_mcast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->bc_destination);
		}
	}

    if (fd <= 0)
	{
	    return -1;
	}

	slen = sendto(fd, p_rtp_data, len, 0, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (slen != len)
	{
		log_print(LOG_ERR, "%s, slen = %d, len = %d, ip=0x%08x\r\n", __FUNCTION__, slen, len, p_rua->ripstr);
	}

	return slen;
}


/**
 * Build audio rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return the rtp packet length, -1 on error
 */
int rtp_audio_build(RCUA * p_rua, uint8 * p_data, int len, uint32 ts, int mbit)
{
	int offset = 0;
	int slen = 0;	
	uint8 * p_rtp_ptr = p_data - 12; // shift forward RTP head

	if (p_rua->rtp_tcp)
	{
		p_rtp_ptr -= 4;
		
		*(p_rtp_ptr+offset) = 0x24; // magic
		offset++;
		*(p_rtp_ptr+offset) = p_rua->bc_interleaved; 	// channel
		offset++;
		*(uint16*)(p_rtp_ptr+offset) = htons(12 + len); // rtp payload length
		offset += 2;
	}

	*(p_rtp_ptr+offset) = (RTP_VERSION << 6);
	offset++;
	*(p_rtp_ptr+offset) = (p_rua->bc_rtp_info.rtp_pt) | ((mbit & 0x01) << 7);
	offset++;
	*(uint16*)(p_rtp_ptr+offset) = htons((uint16)(p_rua->bc_rtp_info.rtp_cnt));
	offset += 2;
	*(uint32*)(p_rtp_ptr+offset) = htonl(p_rua->bc_rtp_info.rtp_ts);
	offset += 4;
	*(uint32*)(p_rtp_ptr+offset) = htonl(p_rua->bc_rtp_info.rtp_ssrc);
	offset += 4;
	
	if (p_rua->rtp_tcp)
	{
		slen = rtp_tcp_tx(p_rua, p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}
	else
	{
		slen = rtp_udp_tx(p_rua, AV_TYPE_BACKCHANNEL, (char *)p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}

	p_rua->bc_rtp_info.rtp_cnt++;
	
	return slen;
}

/**
 * Build audio rtp packet and send (not fragment)
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return the rtp packet length, -1 on error
 */
int rtp_audio_tx(RCUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	int ret = -1;
	uint8 * p_buf = NULL;
	
	p_buf = (uint8 *)malloc(size+32);
	if (p_buf == NULL)
	{
		log_print(LOG_ERR, "%s, malloc failed!!!\r\n", __FUNCTION__);
		return -1;
	}

	uint8 * p_tbuf = p_buf + 32; // shift forword header

	memcpy(p_tbuf, p_data, size);

	ret = rtp_audio_build(p_rua, p_tbuf, size, ts, 1);	

	free(p_buf);

	return ret;
}

void rtsp_bc_cb(uint8 *data, int size, int nbsamples, void *pUserdata)
{
	RCUA * p_rua = (RCUA *)pUserdata;

	if (p_rua->send_bc_data)
	{
		rtp_audio_tx(p_rua, data, size, rtsp_get_timestamp(nbsamples));
	}
}

#endif // BACKCHANNEL



