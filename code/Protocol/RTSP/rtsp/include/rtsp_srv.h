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

#ifndef	_RTSP_SRV_HEADER_
#define	_RTSP_SRV_HEADER_

#include "rtsp_rsua.h"
#include "hqueue.h"

#include <vector>
#include <memory>

/*************************************************************************/
#define NET_IF_NUM		    8
#define SRV_PORT_NUM	    4

#define RTSP_MSG_SRC	    1
#define RTSP_TIMER_SRC	    2
#define RTSP_DEL_UA_SRC	    3
#define RTSP_EXIT           4

#define RTSP_PARSE_FAIL		-1
#define RTSP_PARSE_MOREDATE 0
#define RTSP_PARSE_SUCC		1


/*************************************************************************/
typedef struct rtsp_class
{
	uint32	        sys_init_flag : 8;
	uint32	        sys_timer_run : 1;
	uint32	        flag_reserved : 23;

	int				r_flag;             // network data receive flag
	HQUEUE* msg_queue;		    // message receive queue

	PPSN_CTX* rua_fl;             // rua free list
	PPSN_CTX* rua_ul;             // rua used list

	int				sfd[NET_IF_NUM];    // server listen socket
	uint16	        sport;				// server listen port

	uint32	        local_ip_num;		// local ip numbers, except 127.0.0.1
	char			local_ipstr[NET_IF_NUM][24];
	uint32	        local_ip[NET_IF_NUM];   // network byte order

	char			srv_ver[256];		// happytimesoft rtsp server 1.0

	pthread_t		tid_pkt_rx;         // packet receive thread
	pthread_t		tid_main;           // main task thread
	pthread_t		tid_FpsMonitor;
	uint32	        timer_id;           // timer task
	uint32          session_timeout;    // sesssion timeout, unit is second

#ifdef EPOLL
	int				ep_fd;
	struct epoll_event* ep_events;
	int				ep_event_num;
#endif
}RTSP_CLASS;


/*************************************************************************/
typedef struct rtsp_internal_msg
{
	uint32	        msg_dua;		    // message destination unit
	uint32	        msg_evt;		    // event / command value
	uint32	        msg_src;		    // message type
	int				msg_len;		    // message buffer length
	char* msg_buf;		    // message buffer
} RIMSG;

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************/
int     rtsp_net_listen_init( uint32 addr, uint16 port, int idx );
bool    rtsp_stop();
void*   rtsp_task( void* argv );
void*   rtsp_rx_thread( void* argv );
void*   rtsp_proxies_fps_thread();
int     rtsp_rx_msg( RSUA* p_rua, HRTSP_MSG* rx_msg );
int     rtsp_server_state( RSUA* p_rua, HRTSP_MSG* rx_msg );
void    rtsp_stop_rua( RSUA* p_rua );
void    rtsp_close_rua( RSUA* p_rua );
BOOL    rtsp_tcp_rx( RSUA* p_rua );
int     rtsp_msg_parser( RSUA* p_rua );
void    rtsp_tcp_data_rx( RSUA* p_rua, uint8* lpData, int rlen );
void    rtsp_print_info();

#ifdef __cplusplus
}
#endif

#endif	// _RTSP_SRV_H




