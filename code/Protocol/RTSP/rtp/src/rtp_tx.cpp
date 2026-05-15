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
#include "rtp_tx.h"
#include "mjpeg_tables.h"
#include "mjpeg.h"
#include "util.h"
#include "media_util.h"
#include "rtsp_util.h"

#ifdef RTCP
#include "rtcp.h"
#endif

#ifdef RTSP_OVER_HTTP
#include "http.h"
#endif

#ifdef RTSP_FILE
extern "C"
{
#include <libavutil/intreadwrite.h>
}
#else

#ifndef AV_RB16
#   define AV_RB16(x)                           \
    ((((const uint8*)(x))[0] << 8) |        \
      ((const uint8*)(x))[1])
#endif
#ifndef AV_WB16
#   define AV_WB16(p, darg) do {                \
        unsigned d = (darg);                    \
        ((uint8*)(p))[1] = (d);               \
        ((uint8*)(p))[0] = (d)>>8;            \
    } while(0)
#endif

#endif

/***********************************************************************/

#define CEIL_RSHIFT(a,b)    (-((-(a)) >> (b)))


/***********************************************************************/

#ifdef TX_SHAPER

RTP_TX_SHAPER  g_tx_shaper;

/***********************************************************************/

void rtp_init_tx_shaper()
{
	g_tx_shaper.limit_lock = sys_os_create_mutex();
	g_tx_shaper.limit_tx_rate = 60 * 1024 * 1024;
	g_tx_shaper.limit_time_unit = 40;
	g_tx_shaper.max_in_time_unit = g_tx_shaper.limit_tx_rate / (1000 / g_tx_shaper.limit_time_unit);
	g_tx_shaper.tx_total = 0;
	g_tx_shaper.prev_time = sys_os_get_ms();
}

#endif 	// end of TX_SHAPER

/**
 * Send rtp data by TCP socket
 *
 * @param p_rua rtsp user agent
 * @param p_data rtp data
 * @param len rtp data len
 * @return -1 on error, or the data length has been sent
 */
int rtp_tcp_tx(RSUA * p_rua, void * p_data, int len)
{
	int offset = 0;
    SOCKET fd = p_rua->fd;

#ifdef RTSP_OVER_HTTP
    if (p_rua->rtsp_data)
    {
        HTTPCLN * p_user = (HTTPCLN *) p_rua->rtsp_data;

        fd = p_user->cfd;
    }
#endif

	if (fd <= 0)
	{
	    return -1;
	}

	sys_os_mutex_enter(p_rua->fd_mutex);

	while (p_rua->rtp_tx && offset < len)
	{
		int tlen = send(fd, (const char *)p_data+offset, len-offset, 0);
		if (tlen == (len-offset))
		{
			offset += tlen;
		}
		else
		{
			sys_os_mutex_leave(p_rua->fd_mutex);

			log_print(LOG_ERR, "%s, send failed, fd[%d],tlen[%d,%d],err[%d][%s]!!!\r\n",
				__FUNCTION__, fd, tlen, (len-offset), errno, strerror(errno));
			return -1;
		}
	}

	sys_os_mutex_leave(p_rua->fd_mutex);

	return offset;
}

/**
 * Send rtp data by UDP socket
 *
 * @param p_rua rtsp user agent
 * @param av_t whether video rtp data
 * @param p_data rtp data
 * @param len rtp data len
 * @return -1 on error, or the data length has been sent
 */
int rtp_udp_tx(RSUA * p_rua, int av_t, char * p_rtp_data, int len)
{
	int slen;
	SOCKET fd = 0;
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = p_rua->user_real_ip;

	if (AV_TYPE_VIDEO == av_t)
	{
	    addr.sin_port = htons(p_rua->r_v_port);
	    fd = p_rua->v_udp_fd;

	    if (0 == p_rua->rtp_unicast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->v_destination);
		}
	}
	else if (AV_TYPE_AUDIO == av_t)
	{
		addr.sin_port = htons(p_rua->r_a_port);
		fd = p_rua->a_udp_fd;

		if (0 == p_rua->rtp_unicast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->a_destination);
		}
	}
#ifdef RTSP_METADATA
	else if (AV_TYPE_METADATA == av_t)
	{
	    addr.sin_port = htons(p_rua->r_m_port);
	    fd = p_rua->m_udp_fd;

	    if (0 == p_rua->rtp_unicast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->m_destination);
		}
	}
#endif

    if (fd <= 0)
	{
	    return -1;
	}

#ifdef TX_SHAPER

TX_LIMIT:
	uint32 curtime = sys_os_get_ms();

	sys_os_mutex_enter(g_tx_shaper.limit_lock);
	if (curtime - g_tx_shaper.prev_time >= (uint32)g_tx_shaper.limit_time_unit)
	{
		g_tx_shaper.prev_time = curtime;
		g_tx_shaper.tx_total = 0;
	}
	sys_os_mutex_leave(g_tx_shaper.limit_lock);

	if (g_tx_shaper.tx_total < g_tx_shaper.max_in_time_unit)
	{
		slen = sendto(fd, p_rtp_data, len, 0, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		if (slen != len)
		{
			log_print(LOG_ERR, "%s, slen = %d, len = %d, ip=0x%08x\r\n",
				__FUNCTION__, slen, len, ntohl(p_rua->user_real_ip));
		}
	}
	else
	{
		usleep(10*1000);
		goto TX_LIMIT;
	}

	sys_os_mutex_enter(g_tx_shaper.limit_lock);
	g_tx_shaper.tx_total += slen;
	sys_os_mutex_leave(g_tx_shaper.limit_lock);

#else
	slen = sendto(fd, p_rtp_data, len, 0, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (slen != len)
	{
		log_print(LOG_ERR, "%s, slen = %d, len = %d, ip=0x%08x\r\n",
			__FUNCTION__, slen, len, ntohl(p_rua->user_real_ip));
	}
#endif

	return slen;
}


/**
 * Build video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @mbit rtp marker flag
 * @return the rtp packet length, -1 on error
 */
int rtp_video_build(RSUA * p_rua, uint8 * p_data, int len, uint32 ts, int mbit)
{
	int offset = 0;
	int slen = 0;
	uint8 buff[40];
	uint8 * p_rtp_ptr;

#ifdef RTCP
	rtcp_send_packet(p_rua, AV_TYPE_VIDEO);
#endif

    p_rua->v_rtp_info.rtp_ts = ts;

	if (p_rua->rtp_tcp)
	{
		*(buff+offset) = 0x24; // magic
		offset++;
		*(buff+offset) = p_rua->v_interleaved; 		// channel
		offset++;
#ifdef RTSP_REPLAY
        if (p_rua->replay)
        {
            *(uint16*)(buff+offset) = htons(12 + len + 16); // rtp payload length
        }
        else
        {
            *(uint16*)(buff+offset) = htons(12 + len); 	    // rtp payload length
        }
#else
		*(uint16*)(buff+offset) = htons(12 + len); 	        // rtp payload length
#endif
		offset += 2;
	}

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(buff+offset) = (RTP_VERSION << 6) | (1 << 4);
    }
    else
    {
        *(buff+offset) = (RTP_VERSION << 6);
    }
#else
    *(buff+offset) = (RTP_VERSION << 6);
#endif
	offset++;
	*(buff+offset) = (p_rua->v_rtp_info.rtp_pt) | ((mbit & 0x01) << 7);
	offset++;
	*(uint16*)(buff+offset) = htons((uint16)(p_rua->v_rtp_info.rtp_cnt));
	offset += 2;
	*(uint32*)(buff+offset) = htonl(p_rua->v_rtp_info.rtp_ts);
	offset += 4;
	*(uint32*)(buff+offset) = htonl(p_rua->v_rtp_info.rtp_ssrc);
	offset += 4;

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(uint16*)(buff+offset) = htons(0xABAC);
        offset += 2;
        *(uint16*)(buff+offset) = htons(3);
        offset += 2;

        uint64 ntp_time = rtsp_ntp_time();

        *(uint32*)(buff+offset) = htonl(ntp_time / 1000000);
        offset += 4;
        *(uint32*)(buff+offset) = htonl(((ntp_time % 1000000) << 32) / 1000000);
        offset += 4;

        *(buff+offset) = ((p_rua->v_rep_hdr.t & 0x01) << 4) | ((p_rua->v_rep_hdr.d & 0x01) << 5) |
                         ((p_rua->v_rep_hdr.e & 0x01) << 6) | ((p_rua->v_rep_hdr.c & 0x01) << 7) | p_rua->v_rep_hdr.mbz;
    	offset++;
        *(buff+offset) = p_rua->v_rep_hdr.seq;
    	offset++;
    	*(uint16*)(buff+offset) = htons(p_rua->v_rep_hdr.padding);
    	offset += 2;

    	p_rua->v_rep_hdr.d = 0;
	}
#endif

	p_rtp_ptr = p_data - offset;
	memcpy(p_rtp_ptr, buff, offset);

	if (p_rua->rtp_tcp)
	{
		slen = rtp_tcp_tx(p_rua, p_rtp_ptr, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}
	else
	{
		slen = rtp_udp_tx(p_rua, AV_TYPE_VIDEO, (char *)p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}

    p_rua->v_rtp_info.rtp_cnt++;

#ifdef RTCP
    p_rua->v_rtcp_info.octet_count += len;
    p_rua->v_rtcp_info.packet_count++;
#endif

	return slen;
}

/**
 * Build video rtp packet and send (not fragment)
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_video_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	int ret = 0;
	int len, max_packet_size;
	uint8 * p = p_data;

    max_packet_size = 1460;

    while (size > 0)
    {
        len = max_packet_size;
        if (len > size)
        {
            len = size;
		}

        ret = rtp_video_build(p_rua, p, len, ts, (len == size));
        if (ret < 0)
		{
			break;
		}

        p += len;
        size -= len;
    }

    return ret;
}

/**
 * Judgment and segmentation h264 rtp data into a single package
 *
 * @param fu_flag fragment flag
 * @param fu_s fragment start flag
 * @param fu_e fragment end flag
 * @param len data length
 * @return the fragmention length
 */
int rtp_h264_fu_split(int * fu_flag, int * fu_s, int * fu_e, int len)
{
	if ((*fu_flag) == 0) // have not yet begun fragment
	{
		if (len <= H264_RTP_MAX_LEN) return len;	// Need not be fragmented

		*fu_flag = 1;
		*fu_s = 1;
		*fu_e = 0;

		return H264_RTP_MAX_LEN;
	}
	else // It has begun to fragment
	{
		*fu_s = 0;

		if (len <= H264_RTP_MAX_LEN) // End fragmentation
		{
			*fu_e = 1;
			return len;
		}
		else
		{
			return H264_RTP_MAX_LEN;
		}
	}
}

/**
 * Build and send h264 fragement rtp packet
 *
 * @param p_rua rtsp user agent
 * @nalu_t the NALU type
 * @fu_s fragement start flag
 * @fu_e fragement end start
 * @param p_data payload data
 * @param len payload data length
 * @return the rtp packet length, -1 on error
 */
int rtp_h264_single_fu_build(RSUA * p_rua, uint8 nalu_t, int fu_s, int fu_e, uint8 * p_data, int len, int last)
{
	int mbit = 0;
	int slen;
	int offset = 0;
	uint8 buff[40];
	uint8 * p_rtp_ptr;

#ifdef RTCP
	rtcp_send_packet(p_rua, AV_TYPE_VIDEO);
#endif

	if (p_rua->rtp_tcp)
	{
		*(buff+offset) = 0x24; // magic
		offset++;
		*(buff+offset) = p_rua->v_interleaved; 			// channel
		offset++;
#ifdef RTSP_REPLAY
        if (p_rua->replay)
        {
            *(uint16*)(buff+offset) = htons(12 + 2 + len + 16); // rtp payload length
        }
        else
        {
            *(uint16*)(buff+offset) = htons(12 + 2 + len); 	    // rtp payload length
        }
#else
		*(uint16*)(buff+offset) = htons(12 + 2 + len); 	        // rtp payload length
#endif
		offset += 2;
	}

	if (fu_e == 1)
	{
		mbit = last;
	}
	else
	{
		mbit = 0;
	}

#ifdef RTSP_REPLAY
	if (p_rua->replay)
    {
        *(buff+offset) = (RTP_VERSION << 6) | (1 << 4);
    }
    else
    {
        *(buff+offset) = (RTP_VERSION << 6);
    }
#else
    *(buff+offset) = (RTP_VERSION << 6);
#endif
	offset++;
	*(buff+offset) = (p_rua->v_rtp_info.rtp_pt) | ((mbit & 0x01) << 7);
	offset++;
	*(uint16*)(buff+offset) = htons((uint16)(p_rua->v_rtp_info.rtp_cnt));
	offset += 2;
	*(uint32*)(buff+offset) = htonl(p_rua->v_rtp_info.rtp_ts);
	offset += 4;
	*(uint32*)(buff+offset) = htonl(p_rua->v_rtp_info.rtp_ssrc);
	offset += 4;

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(uint16*)(buff+offset) = htons(0xABAC);
        offset += 2;
        *(uint16*)(buff+offset) = htons(3);
        offset += 2;

        uint64 ntp_time = rtsp_ntp_time();

        *(uint32*)(buff+offset) = htonl(ntp_time / 1000000);
        offset += 4;
        *(uint32*)(buff+offset) = htonl(((ntp_time % 1000000) << 32) / 1000000);
        offset += 4;

        *(buff+offset) = ((p_rua->v_rep_hdr.t & 0x01) << 4) | ((p_rua->v_rep_hdr.d & 0x01) << 5) |
                         ((p_rua->v_rep_hdr.e & 0x01) << 6) | ((p_rua->v_rep_hdr.c & 0x01) << 7) | p_rua->v_rep_hdr.mbz;
    	offset++;
        *(buff+offset) = p_rua->v_rep_hdr.seq;
    	offset++;
    	*(uint16*)(buff+offset) = htons(p_rua->v_rep_hdr.padding);
    	offset += 2;

    	p_rua->v_rep_hdr.d = 0;
	}
#endif

	buff[offset] = (nalu_t & 0x60) | 28;
	offset++;
	buff[offset] = (nalu_t & 0x1F);

	if (fu_s == 1)
	{
		buff[offset] |= 0x80;
	}

	if (fu_e == 1)
	{
		buff[offset] |= 0x40;
	}

	offset++;

	p_rtp_ptr = p_data - offset;
	memcpy(p_rtp_ptr, buff, offset);

	if (p_rua->rtp_tcp)
	{
		slen = rtp_tcp_tx(p_rua, p_rtp_ptr, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}
	else
	{
		slen = rtp_udp_tx(p_rua, AV_TYPE_VIDEO, (char *)p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}

	p_rua->v_rtp_info.rtp_cnt++;

#ifdef RTCP
	p_rua->v_rtcp_info.octet_count += len;
    p_rua->v_rtcp_info.packet_count++;
#endif

	return slen;
}

/**
 * Send h264 rtp packet
 *
 * @param p_rua rtsp user agent
 * @ts the packet timestamp
 * @nalu_t the NALU type
 * @fu_flag fragment flag
 * @fu_s fragement start flag
 * @fu_e fragement end start
 * @param p_data payload data
 * @param len payload data length
 * @return the rtp packet length, -1 on error
 */
int rtp_h264_tx(RSUA * p_rua, uint32 ts, uint8 nalu_t, int fu_flag, int fu_s, int fu_e, uint8 * p_data, int len, int last)
{
	int ret = 0;

	p_rua->v_rtp_info.rtp_ts = ts;

	if (fu_flag == 0)
	{
		ret = rtp_video_build(p_rua, p_data, len, ts, last);
	}
	else
	{
		ret = rtp_h264_single_fu_build(p_rua, nalu_t, fu_s, fu_e, p_data, len, last);
	}

	return ret;
}

/**
 * Build h264 video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @i_flag the I-Frame flag
 * @return 1 on success, -1 on error
 */
int rtp_h264_video_pkt_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts, int last)
{
	int ret = 1;
	int frame_len = len;
	int fu_s = 0, fu_e = 0, fu_flag = 0;	// Fragement start, end flag
	uint8 * p_tx_data = p_data;
	uint8 nalu_t = p_tx_data[0];
	int keyframe = ((nalu_t & 0x1F) == 5);

#ifdef RTSP_REPLAY
    p_rua->v_rep_hdr.c = keyframe;
#endif

	while (frame_len > 0)
	{
		int tlen = rtp_h264_fu_split(&fu_flag, &fu_s, &fu_e, frame_len);
		if (fu_flag == 1)
		{
			if (fu_s == 1)
			{
				p_tx_data++;
				tlen--;
				frame_len--;
			}
		}

		// the sps and pps frame
		if ((nalu_t & 0x1F) == 7 || (nalu_t & 0x1F) == 8)
		{
			ret = rtp_h264_tx(p_rua, ts, nalu_t, fu_flag, fu_s, fu_e, p_tx_data, tlen, last);
			if (ret == -1)
			{
				break;
			}
		}
		else if (p_rua->iframe_tx == 0 && 0 == keyframe)
		{
			// Non-I-frame, not yet sent I-frame, skip P frame
		}
#ifdef RTSP_REPLAY
        if (p_rua->replay && 2 == p_rua->frame && 0 == keyframe)
        {
            // only send I-frame
        }
#endif
		else
		{
			ret = rtp_h264_tx(p_rua, ts, nalu_t, fu_flag, fu_s, fu_e, p_tx_data, tlen, last);
			if (ret == -1)
			{
				break;
			}

			p_rua->iframe_tx = 1;
		}

		p_tx_data += tlen;
		frame_len -= tlen;
	}

	return ret;
}

/**
 * Build h264 video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_h264_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts)
{
	int ret = 1;

	uint8 *r, *end = p_data + len;
	r = avc_find_startcode(p_data, end);

	while (r < end)
	{
        uint8 *r1;

        while (!*(r++));
        r1 = avc_find_startcode(r, end);

		ret = rtp_h264_video_pkt_tx(p_rua, r, r1 - r, ts, r1 == end);
		if (ret < 0)
		{
			break;
		}

        r = r1;
    }

	return ret;
}

int rtp_h265_video_pkt_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts, int last)
{
	int frame_len = len;
	uint8 * p_tx_data = p_data;
	int rtp_payload_size = H265_RTP_MAX_LEN - 3;
	int nal_type = (p_tx_data[0] >> 1) & 0x3F;

	/* send it as one single NAL unit? */
	if (frame_len <= H265_RTP_MAX_LEN)
	{
		rtp_video_build(p_rua, p_tx_data, frame_len, ts, last);
	}
	else
	{
		/*
		create the HEVC payload header and transmit the buffer as fragmentation units (FU)

		0                   1
		0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
		+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		|F|   Type    |  LayerId  | TID |
		+-------------+-----------------+

		F       = 0
		Type    = 49 (fragmentation unit (FU))
		LayerId = 0
		TID     = 1
		*/

		uint8 tbuf[2048];
		uint8 * buf1 = tbuf+32;
		uint8 * buf = p_tx_data;

		buf1[0] = 49 << 1;
		buf1[1] = 1;

		/*
		create the FU header

		0 1 2 3 4 5 6 7
		+-+-+-+-+-+-+-+-+
		|S|E|  FuType   |
		+---------------+

		S       = variable
		E       = variable
		FuType  = NAL unit type
		*/

		buf1[2] = nal_type;
		/* set the S bit: mark as start fragment */
		buf1[2] |= 1 << 7;

		/* pass the original NAL header */
		buf += 2;
		frame_len -= 2;

		while (frame_len > rtp_payload_size)
		{
			memcpy(buf1+3, buf, rtp_payload_size);
			rtp_video_build(p_rua, buf1, H265_RTP_MAX_LEN, ts, 0);

			buf += rtp_payload_size;
			frame_len -= rtp_payload_size;

			/* reset the S bit */
			buf1[2] &= ~(1 << 7);
		}

		/* set the E bit: mark as last fragment */
		buf1[2] |= 1 << 6;
		/* complete and send last RTP packet */
		memcpy(buf1+3, buf, frame_len);

		rtp_video_build(p_rua, buf1, frame_len + 3, ts, last);
	}

	return 1;
}

int rtp_h265_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts)
{
	int ret = -1;

	uint8 *r, *end = p_data + len;
	r = avc_find_startcode(p_data, end);

	while (r < end)
	{
        uint8 *r1;

        while (!*(r++));
        r1 = avc_find_startcode(r, end);

		ret = rtp_h265_video_pkt_tx(p_rua, r, r1 - r, ts, r1 == end);
		if (ret < 0)
		{
			break;
		}

        r = r1;
    }

	return ret;
}


/**
 * Build jpeg video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return the send data length, -1 on error
 */
int rtp_jpeg_video_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	int ret = 0;
	const uint8 *qtables[4] = { NULL };
    int nb_qtables = 0;
    uint8 type;
    uint8 w, h;
    uint8 *p;
    int off = 0; /* fragment offset of the current JPEG frame */
    int len;
    int i;
    int dri = 0;
    int default_huffman_tables = 0;

    /* convert video pixel dimensions from pixels to blocks */
    w = CEIL_RSHIFT(p_rua->media_info.v_width, 3);
    h = CEIL_RSHIFT(p_rua->media_info.v_height, 3);

    type = 1; // YUVJ420P, YUVJ422P is 0

    /* preparse the header for getting some infos */
    for (i = 0; i < size; i++)
    {
        if (p_data[i] != 0xff)
        {
            continue;
		}

        if (p_data[i + 1] == MARKER_DQT)
        {
            int tables, j;

            if (p_data[i + 4] & 0xF0)
            {
            	log_print(H_LOG_WARN, "%s, Only 8-bit precision is supported\r\n", __FUNCTION__);
            }

            /* a quantization table is 64 bytes long */
            tables = AV_RB16(&p_data[i + 2]) / 65;
            if (i + 5 + tables * 65 > size)
            {
            	log_print(LOG_ERR, "%s, Too short JPEG header. Aborted!\r\n", __FUNCTION__);
                return -1;
            }

            if (nb_qtables + tables > 4)
            {
            	log_print(LOG_ERR, "%s, Invalid number of quantisation tables\r\n", __FUNCTION__);
                return -1;
            }

            for (j = 0; j < tables; j++)
            {
                qtables[nb_qtables + j] = p_data + i + 5 + j * 65;
            }
            nb_qtables += tables;
        }
        else if (p_data[i + 1] == MARKER_SOF0)
        {
            if (p_data[i + 14] != 17 || p_data[i + 17] != 17)
            {
                log_print(LOG_ERR, "%s, Only 1x1 chroma blocks are supported. Aborted!\r\n", __FUNCTION__);
                return -1;
            }

            type = (p_data[i + 11] & 1) ? 0 : 1;
        }
        else if (p_data[i + 1] == MARKER_DHT)
        {
            int dht_size = AV_RB16(&p_data[i + 2]);
            default_huffman_tables |= 1 << 4;
            i += 3;
            dht_size -= 2;
            if (i + dht_size >= size)
            {
                continue;
            }

            while (dht_size > 0)
            {
                switch (p_data[i + 1])
                {
                case 0x00:
                    if (   dht_size >= 29
                        && !memcmp(p_data + i +  2, lum_dc_codelens, 16)
                        && !memcmp(p_data + i + 18, lum_dc_symbols, 12))
                    {
                        default_huffman_tables |= 1;
                        i += 29;
                        dht_size -= 29;
                    }
                    else
                    {
                        i += dht_size;
                        dht_size = 0;
                    }
                    break;

                case 0x01:
                    if (   dht_size >= 29
                        && !memcmp(p_data + i +  2, chm_dc_codelens, 16)
                        && !memcmp(p_data + i + 18, lum_dc_symbols, 12))
                    {
                        default_huffman_tables |= 1 << 1;
                        i += 29;
                        dht_size -= 29;
                    }
                    else
                    {
                        i += dht_size;
                        dht_size = 0;
                    }
                    break;

                case 0x10:
                    if (   dht_size >= 179
                        && !memcmp(p_data + i +  2, lum_ac_codelens, 16)
                        && !memcmp(p_data + i + 18, lum_ac_symbols, 162))
                    {
                        default_huffman_tables |= 1 << 2;
                        i += 179;
                        dht_size -= 179;
                    }
                    else
                    {
                        i += dht_size;
                        dht_size = 0;
                    }
                    break;

                case 0x11:
                    if (   dht_size >= 179
                        && !memcmp(p_data + i +  2, chm_ac_codelens, 16)
                        && !memcmp(p_data + i + 18, chm_ac_symbols, 162))
                    {
                        default_huffman_tables |= 1 << 3;
                        i += 179;
                        dht_size -= 179;
                    }
                    else
                    {
                        i += dht_size;
                        dht_size = 0;
                    }
                    break;

                default:
                    i += dht_size;
                    dht_size = 0;
                    continue;
            	}
            }
        }
        else if (p_data[i + 1] == MARKER_DRI)
        {
            dri = p_data[i + 5];
        }
        else if (p_data[i + 1] == MARKER_SOS)
        {
            /* SOS is last marker in the header */
            i += AV_RB16(&p_data[i + 2]) + 2;
            if (i > size)
            {
               	log_print(LOG_ERR, "%s, Insufficient data. Aborted!\r\n", __FUNCTION__);
                return -1;
            }
            break;
        }
    }

    if (default_huffman_tables && default_huffman_tables != 31)
    {
        log_print(LOG_ERR, "%s, RFC 2435 requires standard Huffman tables for jpeg\r\n", __FUNCTION__);
        return -1;
    }

    if (nb_qtables && nb_qtables != 2)
    {
    	// log_print(H_LOG_INFO, "%s, RFC 2435 suggests two quantization tables, %d provided\r\n", __FUNCTION__, nb_qtables);
    }

    /* skip JPEG header */
    p_data += i;
    size -= i;

    for (i = size - 2; i >= 0; i--)
    {
        if (p_data[i] == 0xff && p_data[i + 1] == MARKER_EOI)
        {
            /* Remove the EOI marker */
            size = i;
            break;
        }
    }

	uint8 buff[2048];
    p = buff + 40;

    while (size > 0)
    {
        int hdr_size = 8;

        if (dri > 0)
        {
            type |= 0x40;
            hdr_size += 4;
        }

        if (off == 0 && nb_qtables)
        {
            hdr_size += 4 + 64 * nb_qtables;
		}

        /* payload max in one packet */
        len = MIN_(size, JPEG_RTP_MAX_LEN - hdr_size);

		/* set main header */
		*p = 0;
		p++;
		p[0] = (off >> 16);
		p[1] = (off >> 8);
		p[2] = off;
		p += 3;
		*p = type;
		p++;

		if (nb_qtables)
		{
		    *p = 255;
		}
		else
		{
		    *p = 90;
		}

		p++;
		*p = w;
		p++;
		*p = h;
		p++;

        if (dri > 0)
        {
            // There is also a Restart Marker Header

            p[0] = dri >> 8;
            p[1] = dri & 0xFF;
            p[2] = p[3] = 0xFF;         // F=L=1; Restart Count = 0x3FFF
            p += 4;
        }

        if (off == 0 && nb_qtables)
        {
            /* set quantization tables header */
			*p = 0;
			p++;
			*p = 0;
			p++;
			p[0] = ((64 * nb_qtables) >> 8);
			p[1] = 64 * nb_qtables;
			p += 2;

            for (i = 0; i < nb_qtables; i++)
            {
            	memcpy(p, qtables[i], 64);
    			p += 64;
            }
        }

        /* copy payload data */
        memcpy(p, p_data, len);

        /* marker bit is last packet in frame */
        ret = rtp_video_build(p_rua, buff + 40, len + hdr_size, ts, size == len);
        if (ret < 0)
        {
        	return -1;
        }

        p_data += len;
        size -= len;
        off += len;
        p = buff + 40;
    }

    return off;
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
int rtp_audio_build(RSUA * p_rua, uint8 * p_data, int len, uint32 ts, int mbit)
{
	int offset = 0;
	int slen = 0;
	uint8 buff[40];
	uint8 * p_rtp_ptr;

#ifdef RTCP
	rtcp_send_packet(p_rua, AV_TYPE_AUDIO);
#endif

    p_rua->a_rtp_info.rtp_ts = ts;

	if (p_rua->rtp_tcp)
	{
		*(buff+offset) = 0x24; // magic
		offset++;
		*(buff+offset) = p_rua->a_interleaved; 		// channel
		offset++;
#ifdef RTSP_REPLAY
        if (p_rua->replay)
        {
            *(uint16*)(buff+offset) = htons(12 + len + 16); 	// rtp payload length
        }
        else
        {
            *(uint16*)(buff+offset) = htons(12 + len); 	        // rtp payload length
        }
#else
		*(uint16*)(buff+offset) = htons(12 + len); 	            // rtp payload length
#endif
		offset += 2;
	}

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(buff+offset) = (RTP_VERSION << 6) | (1 << 4);
    }
    else
    {
        *(buff+offset) = (RTP_VERSION << 6);
    }
#else
    *(buff+offset) = (RTP_VERSION << 6);
#endif
	offset++;
	*(buff+offset) = (p_rua->a_rtp_info.rtp_pt) | ((mbit & 0x01) << 7);
	offset++;
	*(uint16*)(buff+offset) = htons((uint16)(p_rua->a_rtp_info.rtp_cnt));
	offset += 2;
	*(uint32*)(buff+offset) = htonl(p_rua->a_rtp_info.rtp_ts);
	offset += 4;
	*(uint32*)(buff+offset) = htonl(p_rua->a_rtp_info.rtp_ssrc);
	offset += 4;

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(uint16*)(buff+offset) = htons(0xABAC);
        offset += 2;
        *(uint16*)(buff+offset) = htons(3);
        offset += 2;

        uint64 ntp_time = rtsp_ntp_time();

        *(uint32*)(buff+offset) = htonl(ntp_time / 1000000);
        offset += 4;
        *(uint32*)(buff+offset) = htonl(((ntp_time % 1000000) << 32) / 1000000);
        offset += 4;

        *(buff+offset) = ((p_rua->a_rep_hdr.t & 0x01) << 4) | ((p_rua->a_rep_hdr.d & 0x01) << 5) |
                         ((p_rua->a_rep_hdr.e & 0x01) << 6) | ((p_rua->a_rep_hdr.c & 0x01) << 7) | p_rua->a_rep_hdr.mbz;
        offset++;
        *(buff+offset) = p_rua->a_rep_hdr.seq;
        offset++;
        *(uint16*)(buff+offset) = htons(p_rua->a_rep_hdr.padding);
        offset += 2;

        p_rua->a_rep_hdr.d = 0;
    }
#endif

	p_rtp_ptr = p_data - offset;
	memcpy(p_rtp_ptr, buff, offset);

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
		slen = rtp_udp_tx(p_rua, AV_TYPE_AUDIO, (char *)p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}

	p_rua->a_rtp_info.rtp_cnt++;

#ifdef RTCP
	p_rua->a_rtcp_info.octet_count += len;
    p_rua->a_rtcp_info.packet_count++;
#endif

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
int rtp_audio_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	int ret = 0;
	int len, max_packet_size;
	uint8 * p = p_data;

    max_packet_size = 1452 - 4 - 12 - 16;

    while (size > 0)
    {
        len = max_packet_size;
        if (len > size)
        {
            len = size;
		}

        ret = rtp_audio_build(p_rua, p, len, ts, (len == size));
        if (ret < 0)
		{
			break;
		}

        p += len;
        size -= len;
    }

    return ret;
}

/**
 * Build AAC audio rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param size payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_aac_audio_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	int ret = 0;
    int len, max_packet_size;
    uint8 *p = p_data - 4;
    int au_size = size;

    max_packet_size = 1452 - 4 - 12 - 16;

    while (size > 0)
    {
        len = MIN_(size, max_packet_size);

        AV_WB16(p, 2 * 8);
    	AV_WB16(&p[2], au_size * 8);

        ret = rtp_audio_build(p_rua, p, len + 4, ts, len == size);
        if (ret < 0)
		{
			break;
		}

        size -= len;
        p += len;
    }

    return ret;
}


#ifdef RTSP_METADATA


int rtp_metadata_build(RSUA * p_rua, uint8 * p_data, int len, uint32 ts, int mbit)
{
	int offset = 0;
	int slen = 0;
	uint8 buff[40];
	uint8 * p_rtp_ptr;

    p_rua->m_rtp_info.rtp_ts = ts;

	if (p_rua->rtp_tcp)
	{
		*(buff+offset) = 0x24; // magic
		offset++;
		*(buff+offset) = p_rua->m_interleaved; 		// channel
		offset++;
#ifdef RTSP_REPLAY
        if (p_rua->replay)
        {
            *(uint16*)(buff+offset) = htons(12 + len + 16); 	// rtp payload length
        }
        else
        {
            *(uint16*)(buff+offset) = htons(12 + len); 	        // rtp payload length
        }
#else
		*(uint16*)(buff+offset) = htons(12 + len); 	            // rtp payload length
#endif
		offset += 2;
	}

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(buff+offset) = (RTP_VERSION << 6) | (1 << 4);
    }
    else
    {
        *(buff+offset) = (RTP_VERSION << 6);
    }
#else
    *(buff+offset) = (RTP_VERSION << 6);
#endif
	offset++;
	*(buff+offset) = (p_rua->m_rtp_info.rtp_pt) | ((mbit & 0x01) << 7);
	offset++;
	*(uint16*)(buff+offset) = htons((uint16)(p_rua->m_rtp_info.rtp_cnt));
	offset += 2;
	*(uint32*)(buff+offset) = htonl(p_rua->m_rtp_info.rtp_ts);
	offset += 4;
	*(uint32*)(buff+offset) = htonl(p_rua->m_rtp_info.rtp_ssrc);
	offset += 4;

#ifdef RTSP_REPLAY
    if (p_rua->replay)
    {
        *(uint16*)(buff+offset) = htons(0xABAC);
        offset += 2;
        *(uint16*)(buff+offset) = htons(3);
        offset += 2;

        uint64 ntp_time = rtsp_ntp_time();

        *(uint32*)(buff+offset) = htonl(ntp_time / 1000000);
        offset += 4;
        *(uint32*)(buff+offset) = htonl(((ntp_time % 1000000) << 32) / 1000000);
        offset += 4;

        *(buff+offset) = ((p_rua->m_rep_hdr.t & 0x01) << 4) | ((p_rua->m_rep_hdr.d & 0x01) << 5) |
                         ((p_rua->m_rep_hdr.e & 0x01) << 6) | ((p_rua->m_rep_hdr.c & 0x01) << 7) | p_rua->m_rep_hdr.mbz;
        offset++;
        *(buff+offset) = p_rua->m_rep_hdr.seq;
        offset++;
        *(uint16*)(buff+offset) = htons(p_rua->m_rep_hdr.padding);
        offset += 2;

        p_rua->m_rep_hdr.d = 0;
    }
#endif

	p_rtp_ptr = p_data - offset;
	memcpy(p_rtp_ptr, buff, offset);

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
		slen = rtp_udp_tx(p_rua, AV_TYPE_METADATA, (char *)p_rtp_ptr, offset+len);
		if (slen != offset+len)
		{
			return -1;
		}
	}

	p_rua->m_rtp_info.rtp_cnt++;

	return slen;
}

int rtp_metadata_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts)
{
	//static int count = 0;
	//printf("send metadata(%d)[%d]\n", count++, size);
	int ret = 0;
	int len, max_packet_size;
	uint8 * p = p_data;

    max_packet_size = 1452 - 4 - 12 - 16;

    while (size > 0)
    {
        len = max_packet_size;
        if (len > size)
        {
            len = size;
		}

        ret = rtp_metadata_build(p_rua, p, len, ts, (len == size));
        if (ret < 0)
		{
			break;
		}

        p += len;
        size -= len;
    }

    return ret;
}

#endif



