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

#ifndef	RTSP_RSUA_H
#define	RTSP_RSUA_H

#include "sys_inc.h"
#include "rtsp_parse.h"
#include "rtsp_media.h"

#ifdef RTSP_BACKCHANNEL
#include "audio_decoder.h"
#include "audio_play.h"
#endif

#include <atomic>

#ifdef RTSP_DEMO
#define MAX_NUM_RUA			4
#else
#   ifdef RTSP_LIVE
#define MAX_NUM_RUA         10
#   else
#define MAX_NUM_RUA			100
#   endif
#endif

#define AV_TYPE_VIDEO       0
#define AV_TYPE_AUDIO       1
#define AV_TYPE_BACKCHANNEL 2
#define AV_TYPE_METADATA    3

typedef enum rtsp_server_states
{
	RSS_NULL = 0,	                            // IDLE
	RSS_OPTIONS,	                            // OPTIONS Message has been received
	RSS_DESCRIBE,	                            // DESCRIBE request is received
	RSS_ANNOUNCE,	                            // ANNOUNCE request is received
	RSS_INIT_V,		                            // Initial state: video channel SETUP request is received
	RSS_INIT_A,		                            // Initial state: Audio channel SETUP request is received
	RSS_READY,		                            // Ready state: SETUP reply received or PAUSE reply received while in playback mode
	RSS_PLAYING,	                            // Play state: Received PLAY, and already replied, is being sent RTP
	RSS_PAUSE,                                  // Pause state: Received PAUSE and already replied
	RSS_RECORDING,	                            // Record state: receive RECORD, and has been replied
} RSSTATE;

typedef struct
{
	uint16			magic;						// 0xABAC
	uint16			length;						// 3
	uint32			ntp_sec;
	uint32			ntp_frac;

	uint32			mbz : 4;					// This field is reserved for future use and must be zero
	uint32			t : 1;						// Indicates that this is the terminal frame on playback of a track. 
												//	A device should signal this flag in both forward and reverse playback whenever no more data is available for a track
	uint32			d : 1;						// Indicates that this access unit follows a discontinuity in transmission.
												//	It is primarily used during reverse replay; 
												//	the first packet of each GOP has the D bit set since it does not chronologically follow the previous packet in the data stream 
	uint32			e : 1;						// Indicates the end of a contiguous section of recording. 
												//	The last access unit in each track before a recording gap, or at the end of available footage, shall have this bit set. 
												// 	When replaying in reverse, the E flag shall be set on the last frame at the end of the contiguous section of recording
	uint32			c : 1;						// Indicates that this access unit is a synchronization point or clean point, 
												//	e.g. the start of an intra-coded frame in the case of video streams
	uint32			seq : 8;					// This is the low-order byte of the Cseq value used in the RTSP PLAY command that was used to initiate transmission. 
												//	When a client sends multiple, consecutive PLAY commands, this value may be used to determine where the data from each new PLAY command begins
	uint32			padding : 16;
} UA_REPLAY_HDR;

typedef struct rtsp_server_ua
{
	uint32	        used_flag	: 1;	        // used flag
	uint32	        rtp_tcp		: 1;	        // whether to use RTP OVER RTSP mode transmission
	uint32	        rtp_unicast	: 1;	        // whether RTP unicast mode
	uint32			rtp_tx		: 1;        // RTP thread starts, is sending RTP
	std::atomic<uint8> stopRtpTx{ 0 };
	uint32			rtp_rx 		: 1;			// RTP receive flag
	uint32	        rtp_pause   : 1;	        // RTP thread pause send data
	uint32	        iframe_tx	: 1;            // if i-frame already sended
	uint32	        play_range	: 1;            // whether the play range is valid
	uint32	        v_setup  	: 1;            // whether the video already be setuped
	uint32	        a_setup  	: 1;            // whether the audio already be setuped
	uint32          m_setup     : 1;            // whether the metadata already be setuped
	uint32	        bc_setup  	: 1;            // whether the back channel already be setuped
	uint32          skip_rtcp   : 1;            // Don't send RTCP sender reports
	uint32          backchannel : 1;            // audio back channel flag
	uint32          ad_inited   : 1;            // audio decoder init flag
	uint32			replay		: 1;			// replay flag
	uint32	        reserved	: 16;
	
	RSSTATE			state;                      // server state
	SOCKET			fd;                         // tcp socket
	SOCKET          v_udp_fd;                   // video udp socket
	SOCKET          a_udp_fd;                   // audio udp socket

    void          * fd_mutex;                   // fd write mutex

#ifdef RTCP
    SOCKET          v_rtcp_fd;                  // video rtcp udp socket
	SOCKET          a_rtcp_fd;                  // audio rtcp udp socket
#endif

	uint32	        cseq;                       // seq no.
	
	time_t          lats_rx_time;               // last received packet time
	
	char			sid[64];		            // session id
	char			uri[256];
	char			cbase[256];		            // Content-Base: rtsp://221.10.50.195:554/broadcast.sdp/

	char			v_ctl[64];                  // video control string
	char			a_ctl[64];                  // audio control string

	uint32	        user_real_ip;	            // user real ip address (network byte order)
	uint16	        user_real_port; 	        // user real port (host byte order)

	uint16	        r_v_port;                   // remote video udp port
	uint16	        r_a_port;                   // remote audio udp port
	uint16	        l_v_port;                   // local video udp port
	uint16	        l_a_port;                   // local audio udp port
	
	uint16	        v_interleaved;	            // video RTP channel values
	uint16	        a_interleaved;	            // audio RTP channel values

    char            v_destination[32];          // video multicast address
    char            a_destination[32];          // audio multicast address
    
	char			rcv_buf[2052];	            // Receive Buffer
	int				rcv_dlen;		            // Already exists in the data buffer length

    int				rtp_t_len;                  // rtp payload total lenght
	int				rtp_rcv_len;                // rtp payload current receive length
	char *			rtp_rcv_buf;                // rtp payload receive buffer
	
	int				self_audio_cap_count;       // Local number of audio capabilities
	uint8	        self_audio_cap[MAX_AVN];    // Local audio capability
	char			self_audio_cap_desc[MAX_AVN][MAX_AVDESCLEN];

	int				self_video_cap_count;       // Local number of video capabilities
	uint8	        self_video_cap[MAX_AVN];    // Local video capability
	char			self_video_cap_desc[MAX_AVN][MAX_AVDESCLEN];

	int				play_range_type;			// 0 - Relative Time, 1 - Absolute time
    double          play_range_begin;           // play range begin
    double          play_range_end;             // play range end
	
	UA_RTP_INFO		a_rtp_info;                 // audio rtp info
	UA_RTP_INFO		v_rtp_info;                 // video rtp info

#ifdef RTCP
    UA_RTCP_INFO    a_rtcp_info;                // audio rtcp info
    UA_RTCP_INFO    v_rtcp_info;                // video rtcp ifo
#endif

	pthread_t		rtp_thread;			        // Send RTP thread ID
    pthread_t       audio_thread;               // audio capture thread ID
    
	UA_MEDIA_INFO   media_info;                 // media information

    HD_AUTH_INFO    auth_info;                  // auth information
    
#ifdef RTSP_BACKCHANNEL
    SOCKET          bc_udp_fd;                  // backchannel udp socket
#ifdef RTCP
    SOCKET          bc_rtcp_fd;                 // backchannel rtcp udp socket
#endif
    char			bc_ctl[64];                 // backchannel control 
    uint16	        bc_interleaved;	            // backchannel RTP channel values
    uint16	        r_bc_port;                  // remote backchannel udp port
	uint16	        l_bc_port;                  // local backchannel udp port

    char            bc_destination[32];         // backchannel multicast address
    
	int				self_bc_cap_count;          // Local number of backchannel capabilities
	uint8	        self_bc_cap[MAX_AVN];       // Local backchannel capability
	char			self_bc_cap_desc[MAX_AVN][MAX_AVDESCLEN];

    int             bc_codec;                   // back channel audio codec
    int             bc_samplerate;              // back channel sample rate
    int             bc_channels;                // back channel channel nums

    RTPRXI          rtprxi;                     // back channel data receiving

    pthread_t       tid_udp_rx;                 // udp data receiving thread
    
	UA_RTP_INFO		bc_rtp_info;                // backchannel rtp info

	CAudioDecoder * audio_decoder;              // audio decoder
	CAudioPlay    * audio_player;               // audio player
#endif // end of RTSP_BACKCHANNEL

#ifdef RTSP_OVER_HTTP
    char            sessioncookie[100];         // rtsp over http session cookie
    void          * rtsp_data;                  // rtsp over http rtsp data connection
    void          * rtsp_cmd;                   // rtsp over http rtsp command connection
#endif

#ifdef RTSP_METADATA
    SOCKET          m_udp_fd;                   // metadata udp socket
#ifdef RTCP
    SOCKET          m_rtcp_fd;                  // metadata rtcp udp socket
#endif

    char            m_ctl[64];                  // metadata control string
    uint16	        m_interleaved;	            // metadata RTP channel values
    uint16	        r_m_port;                   // remote metadata udp port
	uint16	        l_m_port;                   // local metadata udp port

    char            m_destination[32];          // metadata multicast address

    int				self_meta_cap_count;        // Local number of metadata capabilities
	uint8	        self_meta_cap[MAX_AVN];     // Local metadata capability
	char			self_meta_cap_desc[MAX_AVN][MAX_AVDESCLEN];
	
    UA_RTP_INFO		m_rtp_info;                 // metadata rtp info
#endif

#ifdef RTSP_REPLAY
	double			scale;						// scale info
	int				rate_control;				// rate control flag, 
												//	1-the stream is delivered in real time using standard RTP timing mechanisms
												//  0-the stream is delivered as fast as possible, using only the flow control provided by the transport to limit the delivery rate
	int				immediate;					// 1 - immediately start playing from the new location, cancelling any existing PLAY command.
												//	The first packet sent from the new location shall have the D (discontinuity) bit set in its RTP extension header. 
	int				frame;						// 0 - all frames
												// 1 - I-frame and P-frame
												// 2 - I-frame
	int				frame_interval;				// I-frame interval, unit is milliseconds		
	
	UA_REPLAY_HDR	v_rep_hdr;					// audio replay header
	UA_REPLAY_HDR	a_rep_hdr;					// audio replay header

#ifdef RTSP_METADATA
	UA_REPLAY_HDR	m_rep_hdr;					// metadata replay header
#endif

#endif
}RSUA;

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************/
HRTSP_MSG * rua_build_security_response(RSUA * p_rua);
HRTSP_MSG * rua_build_options_response(RSUA * p_rua);
HRTSP_MSG * rua_build_get_parameter_response(RSUA * p_rua);
HRTSP_MSG * rua_build_descibe_response(RSUA * p_rua);
BOOL        rua_build_sdp_msg(RSUA * p_rua, HRTSP_MSG * tx_msg);
int         rtsp_cacl_sdp_length(HRTSP_MSG * tx_msg);

HRTSP_MSG * rua_build_setup_response(RSUA * p_rua, int av_t);
HRTSP_MSG * rua_build_play_response(RSUA * p_rua);

HRTSP_MSG * rua_build_response(RSUA * p_rua, const char * resp_str);
/*************************************************************************/
BOOL        rua_get_transport_info(RSUA * p_rua, char * transport_buf, int av_t);
BOOL        rua_get_play_range_info(RSUA * p_rua, char * range_buf);

#ifdef PUSHER
BOOL        rsua_get_sdp_h264_desc(RSUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rsua_get_sdp_h265_desc(RSUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rsua_get_sdp_mp4_desc(RSUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rsua_get_sdp_mp4_params(RSUA * p_rua, int * pt, char * p_cfg, int max_len);
BOOL        rsua_get_sdp_aac_desc(RSUA * p_rua, int * pt, char * p_sdp, int max_len);
BOOL        rsua_get_sdp_aac_params(RSUA * p_rua, int *pt, int *sizelength, int *indexlength, int *indexdeltalength, char * p_cfg, int max_len);
#endif

/*************************************************************************/
void        rsua_send_rtsp_msg(RSUA * p_rua,HRTSP_MSG * tx_msg);

#define     rsua_send_free_rtsp_msg(p_rua,tx_msg) \
                do { \
                    rsua_send_rtsp_msg(p_rua,tx_msg); \
                    rtsp_free_msg(tx_msg); \
                } while(0)

/*************************************************************************/
void        rua_proxy_init();
void        rua_proxy_deinit();

RSUA *      rua_get_idle_rua();
void        rua_set_idle_rua(RSUA * p_rua);
void        rua_set_online_rua(RSUA * p_rua);

/*************************************************************************/
RSUA *      rua_lookup_start();
RSUA *      rua_lookup_next(RSUA * p_rua);
void        rua_lookup_stop();

/*************************************************************************/
uint32      rua_get_index(RSUA * p_rua);
RSUA *      rua_get_by_index(uint32 index);

#ifdef RTSP_OVER_HTTP
RSUA *      rua_get_by_sessioncookie(char * sessioncookie);
#endif

/*************************************************************************/
BOOL        rsua_init_udp_connection(RSUA * p_rua, int av_t, uint32 lip);
BOOL        rsua_init_mc_connection(RSUA * p_rua, int av_t, uint32 lip);

#ifdef RTCP
BOOL        rsua_init_rtcp_udp_connection(RSUA * p_rua, int av_t, uint32 lip);
BOOL        rsua_init_rtcp_mc_connection(RSUA * p_rua, int av_t, uint32 lip);
#endif


#ifdef __cplusplus
}
#endif

#endif // RTSP_RSUA_H



