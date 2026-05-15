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
#include "rtcp.h"
#include "rtp.h"
#include "util.h"
#include "rtsp_util.h"

#ifdef RTCP

/**************************************************************************************/

/* RTCP packets use 0.5% of the bandwidth */
#define RTCP_TX_RATIO_NUM 	5
#define RTCP_TX_RATIO_DEN 	1000

#define RTCP_SR_SIZE 		28

extern int rtp_tcp_tx(RSUA * p_rua, void * p_data, int len);

/**************************************************************************************/

int rtcp_udp_tx(RSUA * p_rua, int av_t, char * p_rtp_data, int len)
{
	int slen;
	SOCKET fd = 0;
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = p_rua->user_real_ip;
	
	if (AV_TYPE_VIDEO == av_t)
	{	    
	    addr.sin_port = htons(p_rua->r_v_port+1);
	    fd = p_rua->v_rtcp_fd;
	    
	    if (0 == p_rua->rtp_unicast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->v_destination);
		}
	}
	else if (AV_TYPE_AUDIO == av_t)
	{
		addr.sin_port = htons(p_rua->r_a_port+1);
		fd = p_rua->a_rtcp_fd;

		if (0 == p_rua->rtp_unicast)
		{
		    addr.sin_addr.s_addr = inet_addr(p_rua->a_destination);
		}
	}

    if (fd <= 0)
	{
	    return -1;
	}

	slen = sendto(fd, p_rtp_data, len, 0, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (slen != len)
	{
		log_print(LOG_ERR, "rtcp_udp_tx::slen = %d, len = %d, ip=0x%08x\r\n", 
			slen, len, ntohl(p_rua->user_real_ip));
	}

	return slen;
}

int rtcp_video_build(RSUA * p_rua, uint8 * p_data, int len)
{
	int slen = 0;
	int offset = 0;

	if (p_rua->rtp_tcp)
	{
		p_data -= 4;
		
		*(p_data+offset) = 0x24; // magic
		offset++;
		*(p_data+offset) = p_rua->v_interleaved+1; 	// channel
		offset++;
		*(uint16*)(p_data+offset) = htons(len); 	// rtcp payload length
		offset += 2;
	}

	if (p_rua->rtp_tcp)
	{
		slen = rtp_tcp_tx(p_rua, p_data, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}
	else
	{
		slen = rtcp_udp_tx(p_rua, AV_TYPE_VIDEO, (char *)p_data, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}

	return slen;
}

int rtcp_audio_build(RSUA * p_rua, uint8 * p_data, int len)
{
	int slen = 0;
	int offset = 0;
	
	if (p_rua->rtp_tcp)
	{
		p_data -= 4;
		
		*(p_data+offset) = 0x24; // magic
		offset++;
		*(p_data+offset) = p_rua->a_interleaved+1; 	// channel
		offset++;
		*(uint16*)(p_data+offset) = htons(len); 	// rtcp payload length
		offset += 2;
	}

	if (p_rua->rtp_tcp)
	{
		slen = rtp_tcp_tx(p_rua, p_data, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}
	else
	{
		slen = rtcp_udp_tx(p_rua, AV_TYPE_AUDIO, (char *)p_data, offset+len);
		if (slen != (offset+len))
		{
			return -1;
		}
	}

	return slen;
}

static void rtcp_send_video_sr(RSUA * p_rua, int64 ntp_time, int bye)
{    
    int  offset = 0;
    uint8 data[2048];
    uint8 * p_data = data+32;
	uint8 * p_rtcp_ptr = p_data;
	
    memset(data, 0, sizeof(data));
    
    p_rua->v_rtcp_info.last_rtcp_ntp_time = ntp_time;

	*(p_rtcp_ptr+offset) = (RTP_VERSION << 6);
	offset++;
	*(p_rtcp_ptr+offset) = RTCP_SR;
	offset++;
    *(uint16*)(p_rtcp_ptr+offset) = htons(6);
    offset += 2;

    rtcp_sr_t * p_sr = (rtcp_sr_t *) (p_data+offset);

    // Insert the NTP and RTP timestamps for the 'wallclock time':
    struct timeval timeNow;
    gettimeofday(&timeNow, NULL);

    p_sr->ntp_sec = htonl(timeNow.tv_sec + 0x83AA7E80);
    
    double fractionalPart = (timeNow.tv_usec/15625.0)*0x04000000; // 2^32/10^6

    p_sr->ntp_frac = htonl((unsigned)(fractionalPart+0.5));    

    // Begin by converting from "struct timeval" units to RTP timestamp units:
	uint32 timestampIncrement = (90000*timeNow.tv_sec);
	timestampIncrement += (uint32)(90000*(timeNow.tv_usec/1000000.0) + 0.5); // note: rounding  
	
    p_sr->ssrc = htonl(p_rua->v_rtp_info.rtp_ssrc);
    p_sr->ntp_sec = htonl(ntp_time / 1000000);
    p_sr->ntp_frac = htonl(((ntp_time % 1000000) << 32) / 1000000);
    p_sr->rtp_ts = htonl(timestampIncrement);
    p_sr->psent = htonl(p_rua->v_rtcp_info.packet_count);
    p_sr->osent = htonl(p_rua->v_rtcp_info.octet_count);    

    offset += 24;

    strcpy(p_rua->v_rtcp_info.cname, "localhost.localdomain");

    if (p_rua->v_rtcp_info.cname[0] != '\0') 
    {
        int len = strlen(p_rua->v_rtcp_info.cname);
        
        p_rtcp_ptr = p_data+offset;

		*(p_rtcp_ptr+offset) = (RTP_VERSION << 6) + 1; // // version and count
		offset++;
		*(p_rtcp_ptr+offset) = RTCP_SDES;
		offset++;
	    *(uint16*)(p_rtcp_ptr+offset) = htons((7 + len + 3) / 4);
	    offset += 2;

		*(uint32*)(p_rtcp_ptr+offset) = htonl(p_rua->v_rtp_info.rtp_ssrc); // ssrc
		 offset += 4;
		*(p_rtcp_ptr+offset) = 0x01; // CNAME
		offset++;
		*(p_rtcp_ptr+offset) = len;
		offset++;
        
        strcpy((char*)p_data+offset, p_rua->v_rtcp_info.cname);

        offset += len;

        p_data[offset++] = 0; // END
        
        for (len = (7 + len) % 4; len % 4; len++)
        {
            p_data[offset++] = 0;
        }
    }

    rtcp_video_build(p_rua, p_data, offset);
}

static void rtcp_send_audio_sr(RSUA * p_rua, int64 ntp_time, int bye)
{
    int  offset = 0;
    uint8 data[2048];
    uint8 * p_data = data+32;
    uint8 * p_rtcp_ptr = p_data;

    memset(data, 0, sizeof(data));
    
    p_rua->a_rtcp_info.last_rtcp_ntp_time = ntp_time;

	*(p_rtcp_ptr+offset) = (RTP_VERSION << 6);
	offset++;
	*(p_rtcp_ptr+offset) = RTCP_SR;
	offset++;
    *(uint16*)(p_rtcp_ptr+offset) = htons(6);
    offset += 2;

    rtcp_sr_t * p_sr = (rtcp_sr_t *) (p_data+offset);

    // Insert the NTP and RTP timestamps for the 'wallclock time':
    struct timeval timeNow;
    gettimeofday(&timeNow, NULL);

    p_sr->ntp_sec = htonl(timeNow.tv_sec + 0x83AA7E80);
    
    double fractionalPart = (timeNow.tv_usec/15625.0)*0x04000000; // 2^32/10^6

    p_sr->ntp_frac = htonl((unsigned)(fractionalPart+0.5));    

    // Begin by converting from "struct timeval" units to RTP timestamp units:
	uint32 timestampIncrement = (p_rua->media_info.a_samplerate*timeNow.tv_sec);
	timestampIncrement += (uint32)(p_rua->media_info.a_samplerate*(timeNow.tv_usec/1000000.0) + 0.5); // note: rounding  
	
    p_sr->ssrc = htonl(p_rua->a_rtp_info.rtp_ssrc);
    p_sr->ntp_sec = htonl(ntp_time / 1000000);
    p_sr->ntp_frac = htonl(((ntp_time % 1000000) << 32) / 1000000);
    p_sr->rtp_ts = htonl(timestampIncrement);
    p_sr->psent = htonl(p_rua->a_rtcp_info.packet_count);
    p_sr->osent = htonl(p_rua->a_rtcp_info.octet_count);    

    offset += 24;

    strcpy(p_rua->a_rtcp_info.cname, "localhost.localdomain");

    if (p_rua->a_rtcp_info.cname[0] != '\0') 
    {
        int len = strlen(p_rua->a_rtcp_info.cname);

        p_rtcp_ptr = p_data+offset;

		*(p_rtcp_ptr+offset) = (RTP_VERSION << 6) + 1; // version and count
		offset++;
		*(p_rtcp_ptr+offset) = RTCP_SDES;
		offset++;
	    *(uint16*)(p_rtcp_ptr+offset) = htons((7 + len + 3) / 4);
	    offset += 2;

		*(uint32*)(p_rtcp_ptr+offset) = htonl(p_rua->a_rtp_info.rtp_ssrc); // ssrc
		 offset += 4;
		*(p_rtcp_ptr+offset) = 0x01; // CNAME
		offset++;
		*(p_rtcp_ptr+offset) = len;
		offset++;
        
        strcpy((char*)p_data+offset, p_rua->a_rtcp_info.cname);

        offset += len;

        p_data[offset++] = 0; // END
        
        for (len = (7 + len) % 4; len % 4; len++)
        {
            p_data[offset++] = 0;
        }
    }

    rtcp_audio_build(p_rua, p_data, offset);
}


/* send an rtcp sender report packet */
static void rtcp_send_sr(RSUA * p_rua, int av_t, int64_t ntp_time, int bye)
{
    if (AV_TYPE_VIDEO == av_t)
    {
        rtcp_send_video_sr(p_rua, ntp_time, bye);
    }
    else if (AV_TYPE_AUDIO == av_t)
    {
        rtcp_send_audio_sr(p_rua, ntp_time, bye);
    }
    else
    {
        return;
    }
}

void rtcp_send_packet(RSUA * p_rua, int av_t)
{
    if (p_rua->skip_rtcp)
    {
        return;
    }

    UA_RTCP_INFO * p_rtcp;

    if (AV_TYPE_VIDEO == av_t)
    {
        p_rtcp = &p_rua->v_rtcp_info;
    }
    else if (AV_TYPE_AUDIO == av_t)
    {
        p_rtcp = &p_rua->a_rtcp_info;
    }
    else
    {
        return;
    }

    int rtcp_bytes;
    
    rtcp_bytes = ((p_rtcp->octet_count - p_rtcp->last_octet_count) * RTCP_TX_RATIO_NUM) / RTCP_TX_RATIO_DEN;
        
    if (!p_rtcp->first_packet || ((rtcp_bytes >= RTCP_SR_SIZE) && 
                            (rtsp_ntp_time() - p_rtcp->last_rtcp_ntp_time > 5000000))) 
    {
        rtcp_send_sr(p_rua, av_t, rtsp_ntp_time(), 0);
        p_rtcp->last_octet_count = p_rtcp->octet_count;
        p_rtcp->first_packet = 1;
    }
}


#endif // end of RTCP


