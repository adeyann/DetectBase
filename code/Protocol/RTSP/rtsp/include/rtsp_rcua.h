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

#ifndef	RTSP_RCUA_H
#define	RTSP_RCUA_H

#include "rtsp_parse.h"

#ifdef BACKCHANNEL
#if __WINDOWS_OS__
#include "audio_capture_win.h"
#elif defined(ANDROID)
#include "audio_input.h"
#endif
#endif


#define AV_TYPE_VIDEO         0
#define AV_TYPE_AUDIO         1
#define AV_TYPE_BACKCHANNEL   2
#define AV_TYPE_METADATA      3


typedef enum rtsp_client_states
{
	RCS_NULL = 0,	
	RCS_OPTIONS,
	RCS_DESCRIBE,	
	RCS_INIT_V,		
	RCS_INIT_A,	
	RCS_INIT_BC,	
	RCS_INIT_M,
	RCS_READY,		
	RCS_PLAYING,	
	RCS_RECORDING,	
} RCSTATE;

typedef struct rtsp_client_user_agent
{
	uint32	        used_flag	: 1;
	uint32	        rtp_tcp		: 1;
	uint32          mast_flag   : 1;    // use rtp multicast, set by user
	uint32	        rtp_mcast	: 1;    // use rtp multicase, set by stack
	uint32	        rtp_tx		: 1;
	uint32          need_auth   : 1;
	uint32          auth_mode   : 2;    // 0 - baisc; 1 - digest
	uint32 	        gp_cmd      : 1;	// is support get_parameter command
    uint32 	        backchannel : 1;
    uint32 	        send_bc_data: 1;
    uint32          replay      : 1;
    uint32	        reserved	: 20;

	int				state;
	SOCKET			fd;                 // socket handler
	uint32			keepalive_time;			

	char			ripstr[128];		// remote ip
	uint16	        rport;				// rtsp server's port

	uint32	        cseq;               // seq no				
	char			sid[64];			// Session ID
	char			uri[256];			// rtsp://221.10.50.195:554/cctv.sdp
	char			cbase[256];			// Content-Base: rtsp://221.10.50.195:554/broadcast.sdp/

	int				session_timeout;	// session timeout value
	
	char			v_ctl[256];			// a=control:trackID=3
	char			a_ctl[256];			// a=control:trackID=3
    
    
	uint16			v_interleaved;      // tcp video interleaved
	uint16			a_interleaved;		// tcp audio interleaved

    char            v_destination[32];  // video multicast address
    char            a_destination[32];  // audio multicast address

    SOCKET          v_udp_fd;           // video udp socket
	SOCKET          a_udp_fd;           // audio udp socket
	
	uint16	        r_v_port;           // remote video udp port
	uint16	        l_v_port;           // local video udp port
	uint16	        r_a_port;           // remote audio udp port	
	uint16	        l_a_port;           // local audio udp port
	
	char			rcv_buf[2052];		
	int				rcv_dlen;			

	int				rtp_t_len;
	int				rtp_rcv_len;
	char *			rtp_rcv_buf;

	int				self_audio_cap_count;
	uint8			self_audio_cap[MAX_AVN];
	char			self_audio_cap_desc[MAX_AVN][MAX_AVDESCLEN];

	int				self_video_cap_count;
	uint8			self_video_cap[MAX_AVN];
	char			self_video_cap_desc[MAX_AVN][MAX_AVDESCLEN];

	int				remote_audio_cap_count;
	uint8			remote_audio_cap[MAX_AVN];
	char			remote_audio_cap_desc[MAX_AVN][MAX_AVDESCLEN];

	int				remote_video_cap_count;
	uint8			remote_video_cap[MAX_AVN];
	char			remote_video_cap_desc[MAX_AVN][MAX_AVDESCLEN];	

    HD_AUTH_INFO 	auth_info;

#ifdef BACKCHANNEL
    char			bc_ctl[256];        // back channel control string
    uint16          bc_interleaved;     // back channel tcp interleaved

    char            bc_destination[32]; // back channel multicast address
    
    SOCKET          bc_udp_fd;          // backchannel udp socket
    uint16	        r_bc_port;          // remote backchannel udp port
	uint16	        l_bc_port;          // local backchannel udp port
	
    int				remote_bc_cap_count;
	uint8			remote_bc_cap[MAX_AVN];
	char			remote_bc_cap_desc[MAX_AVN][MAX_AVDESCLEN];

    UA_RTP_INFO		bc_rtp_info;     		// audio rtp info

#if __WINDOWS_OS__
    CAudioCapture * audio_captrue;
#elif defined(ANDROID)
    AudioInput    * audio_input;
#endif

#endif

#ifdef METADATA
    char            m_ctl[256];         // metadata control string
    uint16          m_interleaved;		// tcp metadata interleaved

    char            m_destination[32];  // metadata multicast address

    SOCKET          m_udp_fd;           // metadata udp socket
    uint16	        r_m_port;           // remote medaata udp port	
	uint16	        l_m_port;           // local medaata udp port

	int				remote_meta_cap_count;
	uint8			remote_meta_cap[MAX_AVN];
	char			remote_meta_cap_desc[MAX_AVN][MAX_AVDESCLEN];
#endif

#ifdef REPLAY
    uint32          scale_flag          : 1;
    uint32          rate_control_flag   : 1;
    uint32          immediate_flag      : 1;
    uint32          frame_flag          : 1;
    uint32          frame_interval_flag : 1;
    uint32          replay_reserved     : 27;
    
    double			scale;              // scale info
	int				rate_control;       // rate control flag, 
								        //	1-the stream is delivered in real time using standard RTP timing mechanisms
								        //  0-the stream is delivered as fast as possible, using only the flow control provided by the transport to limit the delivery rate
	int				immediate;          // 1 - immediately start playing from the new location, cancelling any existing PLAY command.
							            //	The first packet sent from the new location shall have the D (discontinuity) bit set in its RTP extension header. 
	int				frame;              // 0 - all frames
								        // 1 - I-frame and P-frame
								        // 2 - I-frame
	int				frame_interval;     // I-frame interval, unit is milliseconds	
#endif
} RCUA;


#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************/
HRTSP_MSG * rua_build_describe(RCUA * p_rua);
HRTSP_MSG * rua_build_setup(RCUA * p_rua,int type);
HRTSP_MSG * rua_build_play(RCUA * p_rua);
HRTSP_MSG * rua_build_pause(RCUA * p_rua);
HRTSP_MSG * rua_build_teardown(RCUA * p_rua);
HRTSP_MSG * rua_build_get_parameter(RCUA * p_rua);
HRTSP_MSG * rua_build_options(RCUA * p_rua);

/*************************************************************************/
BOOL 		rua_get_media_info(RCUA * p_rua, HRTSP_MSG * rx_msg);

BOOL        rua_get_sdp_video_desc(RCUA * p_rua, const char * key, int * pt, char * p_sdp, int max_len);
BOOL        rua_get_sdp_audio_desc(RCUA * p_rua, const char * key, int * pt, char * p_sdp, int max_len);

BOOL        rua_get_sdp_h264_desc(RCUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rua_get_sdp_h264_params(RCUA * p_rua, int * pt, char * p_sps_pps, int max_len);

BOOL        rua_get_sdp_h265_desc(RCUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rua_get_sdp_h265_params(RCUA * p_rua, int * pt, BOOL * donfield, char * p_vps, int vps_len, char * p_sps, int sps_len, char * p_pps, int pps_len);

BOOL        rua_get_sdp_mp4_desc(RCUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL 		rua_get_sdp_mp4_params(RCUA * p_rua, int * pt, char * p_cfg, int max_len);

BOOL        rua_get_sdp_aac_desc(RCUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rua_get_sdp_aac_params(RCUA * p_rua, int *pt, int *sizelength, int *indexlength, int *indexdeltalength, char * p_cfg, int max_len);

BOOL        rua_init_udp_connection(RCUA * p_rua, int av_t);
BOOL        rua_init_mc_connection(RCUA * p_rua, int av_t);

/*************************************************************************/
void 		rcua_send_rtsp_msg(RCUA * p_rua,HRTSP_MSG * tx_msg);

#define     rcua_send_free_rtsp_msg(p_rua,tx_msg) \
                do { \
                    rcua_send_rtsp_msg(p_rua,tx_msg); \
                    rtsp_free_msg(tx_msg); \
                } while(0)


#ifdef __cplusplus
}
#endif

#endif	//	RTSP_RCUA_H




