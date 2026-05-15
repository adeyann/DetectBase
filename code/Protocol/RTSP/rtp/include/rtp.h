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

#ifndef	__H_RTP_H__
#define	__H_RTP_H__


#define H264_RTP_MAX_LEN    (1452 - 4 - 12 - 2 - 16)	// PPP TCP IL RTP FU REPLAYHDR
#define JPEG_RTP_MAX_LEN    (1452 - 4 - 12 - 16)
#define H265_RTP_MAX_LEN    (1452 - 4 - 12 - 16)

/*
 * Current protocol version.
 */
#define RTP_VERSION         2

#define RTP_SEQ_MOD         (1<<16)
#define RTP_MAX_SDES        255      /* maximum text length for SDES */


typedef enum {
    RTCP_FIR        = 192,
    RTCP_NACK,      // 193
    RTCP_SMPTETC,   // 194
    RTCP_IJ,        // 195
    RTCP_SR         = 200,
    RTCP_RR,        // 201
    RTCP_SDES,      // 202
    RTCP_BYE,       // 203
    RTCP_APP,       // 204
    RTCP_RTPFB,     // 205
    RTCP_PSFB,      // 206
    RTCP_XR,        // 207
    RTCP_AVB,       // 208
    RTCP_RSI,       // 209
    RTCP_TOKEN,     // 210
} rtcp_type_t;

#define RTP_PT_IS_RTCP(x) (((x) >= RTCP_FIR && (x) <= RTCP_IJ) || \
                           ((x) >= RTCP_SR  && (x) <= RTCP_TOKEN))


/*
 * RTP data header
 */
typedef struct 
{
#if 0	// BIG_ENDIA
    uint32 version:2;   /* protocol version */
    uint32 p:1;         /* padding flag */
    uint32 x:1;         /* header extension flag */
    uint32 cc:4;        /* CSRC count */
    uint32 m:1;         /* marker bit */
    uint32 pt:7;        /* payload type */
	uint32 seq:16;      /* sequence number */

#else

    uint32 cc:4;        /* CSRC count */
    uint32 x:1;         /* header extension flag */
    uint32 p:1;         /* padding flag */
    uint32 version:2;   /* protocol version */
 
	uint32 pt:7;        /* payload type */
    uint32 m:1;         /* marker bit */

	uint32 seq:16;      /* sequence number */

#endif

    uint32 ts;          /* timestamp */
    uint32 ssrc;        /* synchronization source */
    uint32 csrc[1];     /* optional CSRC list */
} rtp_hdr_t;

/*
 * Reception report block
 */
typedef struct {
    uint32 ssrc;        /* data source being reported */
    uint32 fraction:8;  /* fraction lost since last SR/RR */
    uint32 lost:24;     /* cumul. no. pkts lost (signed!) */
    uint32 last_seq;    /* extended last seq. no. received */
    uint32 jitter;      /* interarrival jitter */
    uint32 lsr;         /* last SR packet from this source */
    uint32 dlsr;        /* delay since last SR packet */
} rtcp_rr_t;

typedef struct 
{
    uint32 ssrc;        /* sender generating this report */
    uint32 ntp_sec;     /* NTP timestamp */
    uint32 ntp_frac;
    uint32 rtp_ts;      /* RTP timestamp */
    uint32 psent;       /* packets sent */
    uint32 osent;       /* octets sent */
} rtcp_sr_t;

typedef struct  
{
    uint32 src;         /* first SSRC/CSRC */
    uint8  type;        /* type of item (rtcp_sdes_type_t) */
    uint8  length;      /* length of item (in octets) */
} rtcp_sdes_t;

typedef struct
{
	uint32	magic	: 8;
	uint32	channel	: 8;
	uint32	rtp_len	: 16;
} RILF;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // __H_RTP_H__



