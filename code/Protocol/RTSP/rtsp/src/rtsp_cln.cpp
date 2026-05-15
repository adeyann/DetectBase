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

#include <string.h>
#include <stdio.h>
#include "sys_inc.h"
#include "rtp.h"
#include "rtsp_cln.h"
#include "mpeg4.h"
#include "mpeg4_rtp_rx.h"
#include "bit_vector.h"
#include "base64.h"
#include "rtsp_util.h"

#ifdef BACKCHANNEL
#include "rtsp_backchannel.h"
#endif

#include "MgenLogger.h"
#include <cerrno>

/***************************************************************************************/
void * rtsp_tcp_rx_thread(void * argv)
{
	CRtsp * pRtsp = (CRtsp *)argv;

	pRtsp->tcp_rx_thread();

	return NULL;
}

void * rtsp_udp_rx_thread(void * argv)
{
	CRtsp * pRtsp = (CRtsp *)argv;

	pRtsp->udp_rx_thread();

	return NULL;
}

int video_data_cb(uint8 * p_data, int len, uint32 ts, uint32 seq, void * p_userdata)
{
	CRtsp * pthis = (CRtsp *)p_userdata;

	pthis->rtsp_video_data_cb(p_data, len, ts, seq);

	return 0;
}

int audio_data_cb(uint8 * p_data, int len, uint32 ts, uint32 seq, void * p_userdata)
{
	CRtsp * pthis = (CRtsp *)p_userdata;

	pthis->rtsp_audio_data_cb(p_data, len, ts, seq);

	return 0;
}



/***************************************************************************************/
CRtsp::CRtsp(void)
{
	memset(&m_rua, 0, sizeof(RCUA));
	memset(&m_url, 0, sizeof(m_url));
	memset(&m_ip, 0, sizeof(m_ip));
	m_nport = 554;

	m_rua.session_timeout = 60;

	m_pNotify = NULL;
	m_pUserdata = NULL;
	m_pVideoCB = NULL;
	m_pAudioCB = NULL;
	m_pRedirectCB = NULL;
#ifdef METADATA
	m_pMetadataCB = NULL;
#endif
	m_pMutex = sys_os_create_mutex();

	m_pAudioConfig = NULL;
	m_nAudioConfigLen = 0;

    m_bRunning = TRUE;
	m_tcpRxTid = 0;
	m_udpRxTid = 0;

	m_VideoCodec = VIDEO_CODEC_NONE;
	m_AudioCodec = AUDIO_CODEC_NONE;
	m_nSamplerate = 0;
	m_nChannels = 0;

	memset(&h265rxi, 0, sizeof(H265RXI));
	memset(&aacrxi, 0, sizeof(AACRXI));
	memset(&rtprxi, 0, sizeof(RTPRXI));

#ifdef BACKCHANNEL
    m_bcAudioCodec = AUDIO_CODEC_NONE;
	m_nbcSamplerate = 0;
	m_nbcChannels = 0;
#endif
	ruaStateMutex = sys_os_create_mutex();
}

CRtsp::~CRtsp(void)
{
	rtsp_close();

	if (m_pMutex)
	{
		sys_os_destroy_sig_mutex(m_pMutex);
		m_pMutex = NULL;
	}

	if (ruaStateMutex)
	{
		sys_os_destroy_sig_mutex(ruaStateMutex);
		ruaStateMutex = NULL;
	}
}

BOOL CRtsp::rtsp_client_start()
{
	m_rua.rport = m_nport;
	strcpy(m_rua.ripstr, m_ip);
	strcpy(m_rua.uri, m_url);

	if (rua_init_connect(&m_rua) == FALSE)
	{
		log_print(LOG_ERR, "%s, rua_init_connect fail!!!\r\n", __FUNCTION__);
		return FALSE;
	}

	m_rua.cseq = 1;

	if (!m_rua.mast_flag)
	{
		// It can change UDP(0), but video flagmentation too hard.
		m_rua.rtp_tcp = 1;
	}

	HRTSP_MSG * tx_msg = rua_build_options(&m_rua);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(&m_rua, tx_msg);
	}

	sys_os_mutex_enter(ruaStateMutex);
	this->m_rua.state = RCS_OPTIONS;
	sys_os_mutex_leave(ruaStateMutex);

	return TRUE;
}

BOOL CRtsp::rua_init_connect(RCUA * p_rua)
{
	SOCKET fd = tcp_connect_timeout(get_address_by_name(p_rua->ripstr), p_rua->rport, 5000);
	if (fd > 0)
	{
		int len = 1024*1024;

		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&len, sizeof(int)))
		{
			log_print(LOG_ERR, "%s, setsockopt SO_RCVBUF error!\n", __FUNCTION__);
			return FALSE;
		}

		p_rua->fd = fd;

		return TRUE;
	}

	return FALSE;
}

void CRtsp::rtsp_send_h264_params(RCUA * p_rua)
{
	int pt;
	char sps[1000], pps[1000] = {'\0'};

	if (!rua_get_sdp_h264_params(p_rua, &pt, sps, sizeof(sps)))
	{
		return;
	}

	char * ptr = strchr(sps, ',');
	if (ptr && ptr[1] != '\0')
	{
		*ptr = '\0';
		strcpy(pps, ptr+1);
	}

	uint8 sps_pps[1000];
	sps_pps[0] = 0x0;
	sps_pps[1] = 0x0;
	sps_pps[2] = 0x0;
	sps_pps[3] = 0x1;

	int len = base64_decode(sps, sps_pps+4, sizeof(sps_pps)-4);
	if (len <= 0)
	{
		return;
	}

	sys_os_mutex_enter(m_pMutex);

	if (m_pVideoCB)
	{
		m_pVideoCB(sps_pps, len+4, 0, 0, m_pUserdata); // Not Here
	}

	if (pps[0] != '\0')
	{
		len = base64_decode(pps, sps_pps+4, sizeof(sps_pps)-4);
		if (len > 0)
		{
			if (m_pVideoCB)
			{
				m_pVideoCB(sps_pps, len+4, 0, 0, m_pUserdata); // Not Here
			}
		}
	}

	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::rtsp_send_h265_params(RCUA * p_rua)
{
	int pt;
	char vps[512] = {'\0'}, sps[512] = {'\0'}, pps[512] = {'\0'};

	if (!rua_get_sdp_h265_params(p_rua, &pt, &h265rxi.rxf_don, vps, sizeof(vps)-1, sps, sizeof(sps)-1, pps, sizeof(pps)-1))
	{
		return;
	}

	uint8 buff[1024];
	buff[0] = 0x0;
	buff[1] = 0x0;
	buff[2] = 0x0;
	buff[3] = 0x1;

	sys_os_mutex_enter(m_pMutex);

	if (vps[0] != '\0')
	{
		int len = base64_decode(vps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return;
		}

		if (m_pVideoCB)
		{
			m_pVideoCB(buff, len+4, 0, 0, m_pUserdata);
		}
	}

	if (sps[0] != '\0')
	{
		int len = base64_decode(sps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return;
		}

		if (m_pVideoCB)
		{
			m_pVideoCB(buff, len+4, 0, 0, m_pUserdata);
		}
	}

	if (pps[0] != '\0')
	{
		int len = base64_decode(pps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return;
		}

		if (m_pVideoCB)
		{
			m_pVideoCB(buff, len+4, 0, 0, m_pUserdata);
		}
	}

	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::rtsp_get_mpeg4_config(RCUA * p_rua)
{
	int pt;
	char config[1000];

	if (!rua_get_sdp_mp4_params(p_rua, &pt, config, sizeof(config)-1))
	{
		return;
	}

    uint32  configLen;
	uint8 * configData = mpeg4_parse_config(config, configLen);
    if (configData)
    {
    	mpeg4rxi.hdr_len = configLen;
    	memcpy(mpeg4rxi.p_buf, configData, configLen);

        delete[] configData;
    }
}

void CRtsp::rtsp_get_aac_config(RCUA * p_rua)
{
	int pt = 0;
	int sizelength = 13;
	int indexlength = 3;
	int indexdeltalength = 3;
	char config[128];

	if (rua_get_sdp_aac_params(p_rua, &pt, &sizelength, &indexlength, &indexdeltalength, config, sizeof(config)))
	{
		m_pAudioConfig = mpeg4_parse_config(config, m_nAudioConfigLen);
	}

	aacrxi.size_length = sizelength;
	aacrxi.index_length = indexlength;
	aacrxi.index_delta_length = indexdeltalength;
}


/***********************************************************************
*
* Close RCUA
*
************************************************************************/
void CRtsp::rtsp_client_stop(RCUA * p_rua)
{
	if (p_rua->fd > 0)
	{
		HRTSP_MSG * tx_msg = rua_build_teardown(p_rua);
		if (tx_msg)
		{
			rcua_send_free_rtsp_msg(p_rua,tx_msg);
		}
	}
}

BOOL CRtsp::rtsp_setup_video(RCUA * p_rua)
{
	HRTSP_MSG * tx_msg = NULL;

	sys_os_mutex_enter(ruaStateMutex);
	p_rua->state = RCS_INIT_V;
	sys_os_mutex_leave(ruaStateMutex);

	if (p_rua->rtp_tcp)
	{
	    p_rua->v_interleaved = 0;
	}
	else if (p_rua->rtp_mcast)
	{
		if (p_rua->r_v_port)
        {
        	p_rua->l_v_port = p_rua->r_v_port;
        }
        else
        {
            p_rua->l_v_port = p_rua->r_v_port = rtsp_get_udp_port();
        }
	}
	else
	{
		if (rua_init_udp_connection(p_rua, AV_TYPE_VIDEO) == FALSE)
		{
	   		return FALSE;
	    }
	}

	tx_msg = rua_build_setup(p_rua, AV_TYPE_VIDEO);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(p_rua, tx_msg);
	}

	return TRUE;
}

BOOL CRtsp::rtsp_setup_audio(RCUA * p_rua)
{
	HRTSP_MSG * tx_msg = NULL;

	sys_os_mutex_enter(ruaStateMutex);
	p_rua->state = RCS_INIT_A;
	sys_os_mutex_leave(ruaStateMutex);

	if (p_rua->rtp_tcp)
	{
	    p_rua->a_interleaved = 2;
	}
	else if (p_rua->rtp_mcast)
	{
		if (p_rua->r_a_port)
        {
        	p_rua->l_a_port = p_rua->r_a_port;
        }
        else
        {
            p_rua->l_a_port = p_rua->r_a_port = rtsp_get_udp_port();
        }
	}
	else
	{
		if (rua_init_udp_connection(p_rua, AV_TYPE_AUDIO) == FALSE)
		{
	   		return FALSE;
	    }
	}

	tx_msg = rua_build_setup(p_rua, AV_TYPE_AUDIO);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(p_rua, tx_msg);
	}

	return TRUE;
}

BOOL CRtsp::rtsp_get_transport_info(RCUA * p_rua, HRTSP_MSG * rx_msg, int av_type)
{
	BOOL ret = FALSE;

	if (p_rua->rtp_tcp)
	{
		if (AV_TYPE_VIDEO == av_type)
		{
			ret = rtsp_get_tcp_transport_info(rx_msg, &p_rua->v_interleaved);
		}
		else if (AV_TYPE_AUDIO == av_type)
		{
			ret = rtsp_get_tcp_transport_info(rx_msg, &p_rua->a_interleaved);
		}
#ifdef METADATA
		else if (AV_TYPE_METADATA == av_type)
		{
			ret = rtsp_get_tcp_transport_info(rx_msg, &p_rua->m_interleaved);
		}
#endif
#ifdef BACKCHANNEL
		else if (AV_TYPE_BACKCHANNEL == av_type)
		{
			ret = rtsp_get_tcp_transport_info(rx_msg, &p_rua->bc_interleaved);
		}
#endif
	}
	else if (p_rua->rtp_mcast)
	{
		if (AV_TYPE_VIDEO == av_type)
		{
			ret = rtsp_get_mc_transport_info(rx_msg, p_rua->v_destination, &p_rua->r_v_port);
		}
		else if (AV_TYPE_AUDIO == av_type)
		{
			ret = rtsp_get_mc_transport_info(rx_msg, p_rua->a_destination, &p_rua->r_a_port);
		}
#ifdef METADATA
		else if (AV_TYPE_METADATA == av_type)
		{
			ret = rtsp_get_mc_transport_info(rx_msg, p_rua->m_destination, &p_rua->r_m_port);
		}
#endif
#ifdef BACKCHANNEL
		else if (AV_TYPE_BACKCHANNEL == av_type)
		{
			ret = rtsp_get_mc_transport_info(rx_msg, p_rua->bc_destination, &p_rua->r_bc_port);
		}
#endif
	}
	else
	{
		if (AV_TYPE_VIDEO == av_type)
		{
			ret = rtsp_get_udp_transport_info(rx_msg, &p_rua->l_v_port, &p_rua->r_v_port);
		}
		else if (AV_TYPE_AUDIO == av_type)
		{
			ret = rtsp_get_udp_transport_info(rx_msg, &p_rua->l_a_port, &p_rua->r_a_port);
		}
#ifdef METADATA
		else if (AV_TYPE_METADATA == av_type)
		{
			ret = rtsp_get_udp_transport_info(rx_msg, &p_rua->l_m_port, &p_rua->r_m_port);
		}
#endif
#ifdef BACKCHANNEL
		else if (AV_TYPE_BACKCHANNEL == av_type)
		{
			ret = rtsp_get_udp_transport_info(rx_msg, &p_rua->l_bc_port, &p_rua->r_bc_port);
		}
#endif
    }

    return ret;
}

BOOL CRtsp::rtsp_options_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		// get supported command list
		p_rua->gp_cmd = rtsp_is_support_get_parameter_cmd(rx_msg);

		p_rua->cseq++;

		tx_msg = rua_build_describe(p_rua);
		if (tx_msg)
		{
			rcua_send_free_rtsp_msg(p_rua, tx_msg);
		}

		sys_os_mutex_enter(ruaStateMutex);
		p_rua->state = RCS_DESCRIBE;
		sys_os_mutex_leave(ruaStateMutex);
	}
	else if (rx_msg->msg_sub_type == 401 && !p_rua->need_auth)
	{
	    p_rua->need_auth = TRUE;

	    if (rtsp_get_digest_info(rx_msg, &(p_rua->auth_info)))
		{
		    p_rua->auth_mode = 1;
			sprintf(p_rua->auth_info.auth_uri, "%s", p_rua->uri);
		}
		else
		{
		    p_rua->auth_mode = 0;
		}

		p_rua->cseq++;

		tx_msg = rua_build_options(p_rua);
    	if (tx_msg)
    	{
    		rcua_send_free_rtsp_msg(p_rua, tx_msg);
    	}
	}
	else if (rx_msg->msg_sub_type == 401 && p_rua->need_auth)
	{
	    send_notify(RTSP_EVE_AUTHFAILED);
	    return FALSE;
	}
	else
	{
	    return FALSE;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_describe_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		// Session
		rtsp_get_session_info(rx_msg, p_rua->sid, sizeof(p_rua->sid)-1, &p_rua->session_timeout);

		char cseq_buf[32];
		char cbase[256];

		rtsp_get_msg_cseq(rx_msg, cseq_buf, sizeof(cseq_buf)-1);

		// Content-Base
		if (rtsp_get_cbase_info(rx_msg, cbase, sizeof(cbase)-1))
		{
			strncpy(p_rua->uri, cbase, sizeof(p_rua->uri)-1);
		}

		rtsp_find_sdp_control(rx_msg, p_rua->v_ctl, "video", sizeof(p_rua->v_ctl)-1);

#ifdef BACKCHANNEL
		if (p_rua->backchannel)
		{
			p_rua->backchannel = rtsp_find_sdp_control(rx_msg, p_rua->bc_ctl, "audio", sizeof(p_rua->bc_ctl)-1, "sendonly");
			if (p_rua->backchannel)
			{
				rtsp_find_sdp_control(rx_msg, p_rua->a_ctl, "audio", sizeof(p_rua->a_ctl)-1, "recvonly");
			}
			else
			{
				rtsp_find_sdp_control(rx_msg, p_rua->a_ctl, "audio", sizeof(p_rua->a_ctl)-1);
			}
		}
		else
		{
			rtsp_find_sdp_control(rx_msg, p_rua->a_ctl, "audio", sizeof(p_rua->a_ctl)-1);
		}
#else
		rtsp_find_sdp_control(rx_msg, p_rua->a_ctl, "audio", sizeof(p_rua->a_ctl)-1);
#endif

#ifdef METADATA
		rtsp_find_sdp_control(rx_msg, p_rua->m_ctl, "application", sizeof(p_rua->m_ctl)-1);
#endif

		if (rua_get_media_info(p_rua, rx_msg))
		{
			rtsp_get_video_media_info();
			rtsp_get_audio_media_info();

#ifdef BACKCHANNEL
			if (p_rua->backchannel)
			{
				rtsp_get_bc_media_info();
			}
#endif
		}

		if (p_rua->mast_flag)
		{
			p_rua->rtp_mcast = rtsp_is_support_mcast(rx_msg);
		}

        p_rua->cseq++;

		// Send SETUP
		if (p_rua->v_ctl[0] != '\0')
		{
			if (!rtsp_setup_video(p_rua))
			{
				return FALSE;
			}
		}
		else if (p_rua->a_ctl[0] != '\0')
		{
			if (!rtsp_setup_audio(p_rua))
			{
				return FALSE;
			}
		}
		else
		{
			return FALSE;
		}
	}
	else if (rx_msg->msg_sub_type == 302)
	{
		log_print(LOG_DBG, "%s, msg type is 302 look for redirect location header\n", __FUNCTION__);
		//printf("%s, msg type is 302 look for redirect location header\n", __FUNCTION__);
		char location_buf[256];
		if (rtsp_get_headline_string(rx_msg, "Location", location_buf, sizeof(location_buf)) == FALSE)
		{
			log_print(LOG_DBG, "%s, failed to find location header for redirect\n", __FUNCTION__);
			//printf("%s, failed to find location header for redirect\n", __FUNCTION__);
			return FALSE;
		}
		sys_os_mutex_enter(ruaStateMutex);
		m_rua.state = RCS_NULL;
		sys_os_mutex_leave(ruaStateMutex);
		log_print(LOG_DBG, "%s, location header is %s\n", __FUNCTION__, location_buf);
		//printf("%s, location header is %s\n", __FUNCTION__, location_buf);

		//char* username = NULL;
		//char* password = NULL;
		//char* address = NULL;
		//int   urlPortNum = 554;

		//if (!parse_url(location_buf, username, password, address, urlPortNum, NULL))
		//{
		//	return FALSE;
		//}
		char* token = strtok(location_buf, "?");

		strcpy(m_url, token);

		//strncpy(m_ip, address, sizeof(m_ip) - 1);
		//delete[] address;
		send_redirect_info(m_url);

		//rtsp_close();
		//rtsp_start(location_buf, NULL, NULL);
		return FALSE;
	}
	else if (rx_msg->msg_sub_type == 401 && !p_rua->need_auth)
	{
	    p_rua->need_auth = TRUE;

	    if (rtsp_get_digest_info(rx_msg, &(p_rua->auth_info)))
		{
		    p_rua->auth_mode = 1;
			sprintf(p_rua->auth_info.auth_uri, "%s", p_rua->uri);
		}
		else
		{
		    p_rua->auth_mode = 0;
		}

		p_rua->cseq++;

		tx_msg = rua_build_describe(p_rua);
    	if (tx_msg)
    	{
    		rcua_send_free_rtsp_msg(p_rua, tx_msg);
    	}
	}
	else if (rx_msg->msg_sub_type == 401 && p_rua->need_auth)
	{
	    send_notify(RTSP_EVE_AUTHFAILED);
	    return FALSE;
	}
#ifdef BACKCHANNEL
	else if (p_rua->backchannel) // the server don't support backchannel
	{
		p_rua->backchannel = 0;

		p_rua->cseq++;

		tx_msg = rua_build_describe(p_rua);
		if (tx_msg)
		{
			rcua_send_free_rtsp_msg(p_rua, tx_msg);
		}

		p_rua->state = RCS_DESCRIBE;
	}
#endif
    else
    {
        return FALSE;
    }

	return TRUE;
}

BOOL CRtsp::rtsp_setup_video_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		char cbase[256];

		// Content-Base
		if (rtsp_get_cbase_info(rx_msg, cbase, sizeof(cbase)-1))
		{
			strncpy(p_rua->uri, cbase, sizeof(p_rua->uri)-1);
			sprintf(p_rua->auth_info.auth_uri, "%s", p_rua->uri);
		}

		// Session
		if (p_rua->sid[0] == '\0')
		{
			rtsp_get_session_info(rx_msg, p_rua->sid, sizeof(p_rua->sid)-1, &p_rua->session_timeout);
        }

		if (!rtsp_get_transport_info(p_rua, rx_msg, AV_TYPE_VIDEO))
		{
			return FALSE;
		}

        if (p_rua->rtp_mcast)
		{
			if (rua_init_mc_connection(p_rua, AV_TYPE_VIDEO) == FALSE)
			{
		   		return FALSE;
		    }
		}

		if (p_rua->sid[0] == '\0')
		{
			sprintf(p_rua->sid, "%x%x", rand(), rand());
		}

		p_rua->cseq++;

		if (p_rua->a_ctl[0] != '\0')
		{
			if (!rtsp_setup_audio(p_rua))
			{
				return FALSE;
			}
		}
#ifdef METADATA
		else if (p_rua->m_ctl[0] != '\0')
		{
			if (!rtsp_setup_metadata(p_rua))
			{
				return FALSE;
			}
		}
#endif
#ifdef BACKCHANNEL
		else if (p_rua->bc_ctl[0] != '\0')
		{
			if (!rtsp_setup_backchannel(p_rua))
			{
				return FALSE;
			}
		}
#endif
		else
		{
		    if (make_prepare_play() == FALSE)
		    {
		        return FALSE;
		    }

			// only video without audio
			tx_msg = rua_build_play(p_rua);
			if (tx_msg)
			{
				rcua_send_free_rtsp_msg(p_rua,tx_msg);
			}

			sys_os_mutex_enter(ruaStateMutex);
			p_rua->state = RCS_READY;
			sys_os_mutex_leave(ruaStateMutex);
		}
	}
	else if (p_rua->rtp_tcp) // maybe the server don't support rtp over tcp, try rtp over udp
	{
		p_rua->rtp_tcp = 0;

        p_rua->cseq++;

	    if (!rtsp_setup_video(p_rua))
		{
			return FALSE;
		}
	}
	else
	{
	    return FALSE;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_setup_audio_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		// Session
		if (p_rua->sid[0] == '\0')
		{
			rtsp_get_session_info(rx_msg, p_rua->sid, sizeof(p_rua->sid)-1, &p_rua->session_timeout);
        }

        if (p_rua->sid[0] == '\0')
		{
			sprintf(p_rua->sid, "%x%x", rand(), rand());
		}

		if (!rtsp_get_transport_info(p_rua, rx_msg, AV_TYPE_AUDIO))
		{
			return FALSE;
		}

        if (p_rua->rtp_mcast)
		{
			if (rua_init_mc_connection(p_rua, AV_TYPE_AUDIO) == FALSE)
			{
		   		return FALSE;
		    }
		}

		p_rua->cseq++;

#ifdef METADATA
		if (p_rua->m_ctl[0] != '\0')
		{
			if (!rtsp_setup_metadata(p_rua))
			{
				return FALSE;
			}
		}
		else
#endif
#ifdef BACKCHANNEL
		if (p_rua->bc_ctl[0] != '\0')
		{
			if (!rtsp_setup_backchannel(p_rua))
			{
				return FALSE;
			}
		}
		else
#endif
		{
		    if (make_prepare_play() == FALSE)
		    {
		        return FALSE;
		    }

			tx_msg = rua_build_play(p_rua);
			if (tx_msg)
			{
				rcua_send_free_rtsp_msg(p_rua,tx_msg);
			}

			sys_os_mutex_enter(ruaStateMutex);
			p_rua->state = RCS_READY;
			sys_os_mutex_leave(ruaStateMutex);
		}
	}
	else if (p_rua->rtp_tcp) // maybe the server don't support rtp over tcp, try rtp over udp
	{
		p_rua->rtp_tcp = 0;

        p_rua->cseq++;

	    if (!rtsp_setup_audio(p_rua))
		{
			return FALSE;
		}
	}
	else
	{
		// error handle

		return FALSE;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_play_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	if (rx_msg->msg_sub_type == 200)
	{
		sys_os_mutex_enter(ruaStateMutex);
		p_rua->state = RCS_PLAYING;
		sys_os_mutex_leave(ruaStateMutex);

		p_rua->keepalive_time = sys_os_get_ms();

		log_print(LOG_DBG, "%s, session timeout : %d\n", __FUNCTION__, p_rua->session_timeout);

		if (m_AudioCodec == AUDIO_CODEC_AAC)
		{
			rtsp_get_aac_config(p_rua);
		}

		send_notify(RTSP_EVE_CONNSUCC);

		if (m_VideoCodec == VIDEO_CODEC_H264)
		{
			rtsp_send_h264_params(p_rua);
		}
		else if (m_VideoCodec == VIDEO_CODEC_MP4)
		{
			rtsp_get_mpeg4_config(p_rua);
		}
		else if (m_VideoCodec == VIDEO_CODEC_H265)
		{
			rtsp_send_h265_params(p_rua);
		}

#ifdef BACKCHANNEL
		if (p_rua->backchannel)
		{
			rtsp_init_backchannel(p_rua);
		}
#endif
	}
	else
	{
		//error handle
		return FALSE;
	}

	return TRUE;
}

#ifdef METADATA

BOOL CRtsp::rtsp_setup_metadata_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		// Session
		if (p_rua->sid[0] == '\0')
		{
			rtsp_get_session_info(rx_msg, p_rua->sid, sizeof(p_rua->sid)-1, &p_rua->session_timeout);
        }

        if (p_rua->sid[0] == '\0')
		{
			sprintf(p_rua->sid, "%x%x", rand(), rand());
		}

		if (!rtsp_get_transport_info(p_rua, rx_msg, AV_TYPE_METADATA))
		{
			return FALSE;
		}

        if (p_rua->rtp_mcast)
		{
			if (rua_init_mc_connection(p_rua, AV_TYPE_METADATA) == FALSE)
			{
		   		return FALSE;
		    }
		}

		p_rua->cseq++;

	    if (make_prepare_play() == FALSE)
	    {
	        return FALSE;
	    }

		tx_msg = rua_build_play(p_rua);
		if (tx_msg)
		{
			rcua_send_free_rtsp_msg(p_rua,tx_msg);
		}

		p_rua->state = RCS_READY;
	}
	else
	{
		// error handle

		return FALSE;
	}

	return TRUE;
}

void CRtsp::set_metadata_cb(metadata_cb cb)
{
	sys_os_mutex_enter(m_pMutex);
	m_pMetadataCB = cb;
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::metadata_rtp_rx(uint8 * lpData, int rlen, unsigned seq, unsigned ts)
{
	sys_os_mutex_enter(m_pMutex);

	if (m_pMetadataCB)
    {
        m_pMetadataCB(lpData, rlen, ts, seq, m_pUserdata);
    }

    sys_os_mutex_leave(m_pMutex);
}

BOOL CRtsp::rtsp_setup_metadata(RCUA * p_rua)
{
	HRTSP_MSG * tx_msg = NULL;

	p_rua->state = RCS_INIT_M;

	if (p_rua->rtp_tcp)
	{
	    p_rua->m_interleaved = 6;
	}
	else if (p_rua->rtp_mcast)
	{
		if (p_rua->r_m_port)
        {
        	p_rua->l_m_port = p_rua->r_m_port;
        }
        else
        {
            p_rua->l_m_port = p_rua->r_m_port = rtsp_get_udp_port();
        }
	}
	else
	{
		if (rua_init_udp_connection(p_rua, AV_TYPE_METADATA) == FALSE)
		{
	   		return FALSE;
	    }
	}

	tx_msg = rua_build_setup(p_rua, AV_TYPE_METADATA);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(p_rua, tx_msg);
	}

	return TRUE;
}

#endif // end of METADATA

#ifdef BACKCHANNEL

int CRtsp::get_bc_flag()
{
	return m_rua.backchannel;
}

void CRtsp::set_bc_flag(int flag)
{
	m_rua.backchannel = flag;
}

int CRtsp::get_bc_data_flag()
{
	if (m_rua.backchannel)
	{
		return m_rua.send_bc_data;
	}

	return -1; // unsupport backchannel
}

void CRtsp::set_bc_data_flag(int flag)
{
	m_rua.send_bc_data = flag;
}

BOOL CRtsp::rtsp_setup_backchannel(RCUA * p_rua)
{
	HRTSP_MSG * tx_msg = NULL;

	p_rua->state = RCS_INIT_BC;

	if (p_rua->rtp_tcp)
	{
	    p_rua->bc_interleaved = 4;
	}
	else if (p_rua->rtp_mcast)
	{
		if (p_rua->r_bc_port)
        {
        	p_rua->l_bc_port = p_rua->r_bc_port;
        }
        else
        {
            p_rua->l_bc_port = p_rua->r_bc_port = rtsp_get_udp_port();
        }
	}
	else
	{
		if (rua_init_udp_connection(p_rua, AV_TYPE_BACKCHANNEL) == FALSE)
		{
	   		return FALSE;
	    }
	}

	tx_msg = rua_build_setup(p_rua, AV_TYPE_BACKCHANNEL);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(p_rua, tx_msg);
	}

	return TRUE;
}

BOOL CRtsp::rtsp_setup_backchannel_res(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
	HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_sub_type == 200)
	{
		// Session
		if (p_rua->sid[0] == '\0')
		{
			rtsp_get_session_info(rx_msg, p_rua->sid, sizeof(p_rua->sid)-1, &p_rua->session_timeout);
        }

        if (p_rua->sid[0] == '\0')
		{
			sprintf(p_rua->sid, "%x%x", rand(), rand());
		}

		if (!rtsp_get_transport_info(p_rua, rx_msg, AV_TYPE_BACKCHANNEL))
		{
			return FALSE;
		}

        if (p_rua->rtp_mcast)
		{
			if (rua_init_mc_connection(p_rua, AV_TYPE_BACKCHANNEL) == FALSE)
			{
		   		return FALSE;
		    }
		}

		p_rua->bc_rtp_info.rtp_cnt = 0;
		p_rua->bc_rtp_info.rtp_pt = p_rua->remote_bc_cap[0];
		p_rua->bc_rtp_info.rtp_ssrc = rand();

		p_rua->cseq++;

#ifdef METADATA
		if (p_rua->m_ctl[0] != '\0')
		{
			if (!rtsp_setup_metadata(p_rua))
			{
				return FALSE;
			}
		}
		else
#endif
		{
			if (make_prepare_play() == FALSE)
		    {
		        return FALSE;
		    }

			tx_msg = rua_build_play(p_rua);
			if (tx_msg)
			{
				rcua_send_free_rtsp_msg(p_rua, tx_msg);
			}

			p_rua->state = RCS_READY;
		}
	}
	else
	{
		// error handle

		return FALSE;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_get_bc_media_info()
{
	if (m_rua.remote_bc_cap_count == 0)
	{
		return FALSE;
	}

	if (m_rua.remote_bc_cap[0] == 0)
	{
		m_bcAudioCodec = AUDIO_CODEC_G711U;
	}
	else if (m_rua.remote_bc_cap[0] == 8)
	{
		m_bcAudioCodec = AUDIO_CODEC_G711A;
	}
	else if (m_rua.remote_bc_cap[0] == 2)
	{
		m_bcAudioCodec = AUDIO_CODEC_G726;
	}
	else if (m_rua.remote_bc_cap[0] == 9)
	{
		m_bcAudioCodec = AUDIO_CODEC_G722;
	}

	int i;
	int rtpmap_len = strlen("a=rtpmap:");

	for (i=0; i<MAX_AVN; i++)
	{
		char * ptr = m_rua.remote_bc_cap_desc[i];

		if (memcmp(ptr, "a=rtpmap:", rtpmap_len) == 0)
		{
			char pt_buf[16];
			char code_buf[64];
			int next_offset = 0;

			ptr += rtpmap_len;

			if (GetLineWord(ptr, 0, strlen(ptr), pt_buf, sizeof(pt_buf), &next_offset, WORD_TYPE_NUM) == FALSE)
			{
				return FALSE;
			}

			GetLineWord(ptr, next_offset, strlen(ptr)-next_offset, code_buf, sizeof(code_buf),  &next_offset, WORD_TYPE_STRING);

			uppercase(code_buf);

			if (strstr(code_buf, "G726-32"))
			{
				m_bcAudioCodec = AUDIO_CODEC_G726;
			}
			else if (strstr(code_buf, "G722"))
			{
				m_bcAudioCodec = AUDIO_CODEC_G722;
			}
			else if (strstr(code_buf, "PCMU"))
			{
				m_bcAudioCodec = AUDIO_CODEC_G711U;
			}
			else if (strstr(code_buf, "PCMA"))
			{
				m_bcAudioCodec = AUDIO_CODEC_G711A;
			}
			else if (strstr(code_buf, "MPEG4-GENERIC"))
			{
				m_bcAudioCodec = AUDIO_CODEC_AAC;
			}
			else if (strstr(code_buf, "OPUS"))
			{
				m_bcAudioCodec = AUDIO_CODEC_OPUS;
			}

			char * p = strchr(code_buf, '/');
			if (p)
			{
				p++;

				char * p1 = strchr(p, '/');
				if (p1)
				{
					*p1 = '\0';
					m_nbcSamplerate = atoi(p);

					p1++;
					if (p1 && *p1 != '\0')
					{
						m_nbcChannels = atoi(p1);
					}
					else
					{
						m_nbcChannels = 1;
					}
				}
				else
				{
					m_nbcSamplerate = atoi(p);
					m_nbcChannels = 1;
				}
			}

			break;
		}
	}

    if (m_bcAudioCodec == AUDIO_CODEC_G722)
    {
        m_nbcSamplerate = 16000;
        m_nbcChannels = 1;
    }

	return TRUE;
}

BOOL CRtsp::rtsp_init_backchannel(RCUA * p_rua)
{
#if __WINDOWS_OS__
	if (CWAudioCapture::getDeviceNums() <= 0)
	{
		return FALSE;
	}

	p_rua->audio_captrue = CWAudioCapture::getInstance(0);
	if (NULL == p_rua->audio_captrue)
	{
		return FALSE;
	}

	p_rua->audio_captrue->addCallback(rtsp_bc_cb, p_rua);
	p_rua->audio_captrue->initCapture(m_bcAudioCodec, m_nbcSamplerate, m_nbcChannels, 0);

	return p_rua->audio_captrue->startCapture();
#elif defined(ANDROID)
    p_rua->audio_input = new AudioInput;
	if (NULL == p_rua->audio_input)
	{
		return FALSE;
	}

	p_rua->audio_input->addCallback(rtsp_bc_cb, p_rua);
	p_rua->audio_input->init(m_bcAudioCodec, m_nbcSamplerate, m_nbcChannels);

	return p_rua->audio_input->start();
#else
	return FALSE;
#endif
}

#endif // end of BACKCHANNEL

#ifdef REPLAY

int CRtsp::get_replay_flag()
{
    return m_rua.replay;
}

void CRtsp::set_replay_flag(int flag)
{
    m_rua.replay = flag;
}

void CRtsp::set_scale(double scale)
{
    if (scale != 0)
    {
        m_rua.scale_flag = 1;
        m_rua.scale = scale;
    }
    else
    {
        m_rua.scale_flag = 0;
        m_rua.scale = 0;
    }
}

void CRtsp::set_rate_control_flag(int flag)
{
    m_rua.rate_control_flag = 1;
    m_rua.rate_control = flag;
}

void CRtsp::set_immediate_flag(int flag)
{
    m_rua.immediate_flag = 1;
    m_rua.immediate = flag;
}

void CRtsp::set_frames_flag(int flag, int interval)
{
    if (flag == 1 || flag == 2)
    {
        m_rua.frame_flag = 1;
        m_rua.frame = flag;
    }
    else
    {
        m_rua.frame_flag = 0;
        m_rua.frame = 0;
    }

    if (interval > 0)
    {
        m_rua.frame_interval_flag = 1;
        m_rua.frame_interval = interval;
    }
    else
    {
        m_rua.frame_interval_flag = 0;
        m_rua.frame_interval = 0;
    }
}

#endif // end of REPLAY

BOOL CRtsp::rtsp_client_state(RCUA * p_rua, HRTSP_MSG * rx_msg)
{
    BOOL ret = TRUE;
	//HRTSP_MSG * tx_msg = NULL;

	if (rx_msg->msg_type == 0)	// Request message?
	{
		return FALSE;
	}

	int ruaState = 0;
	sys_os_mutex_enter(ruaStateMutex);
	ruaState = p_rua->state;
	sys_os_mutex_leave(ruaStateMutex);

	switch (ruaState)
	{
	case RCS_NULL:
		break;

	case RCS_OPTIONS:
		ret = rtsp_options_res(p_rua, rx_msg);
		break;

	case RCS_DESCRIBE:
		ret = rtsp_describe_res(p_rua, rx_msg);
		break;

	case RCS_INIT_V:
		ret = rtsp_setup_video_res(p_rua, rx_msg);
		break;

	case RCS_INIT_A:
		ret = rtsp_setup_audio_res(p_rua, rx_msg);
		break;

#ifdef BACKCHANNEL
	case RCS_INIT_BC:
		ret = rtsp_setup_backchannel_res(p_rua, rx_msg);
		break;
#endif

#ifdef METADATA
	case RCS_INIT_M:
		ret = rtsp_setup_metadata_res(p_rua, rx_msg);
		break;
#endif

	case RCS_READY:
		ret = rtsp_play_res(p_rua, rx_msg);
		break;

	case RCS_PLAYING:
		break;

	case RCS_RECORDING:
		break;
	}

	return ret;
}

BOOL CRtsp::make_prepare_play()
{
	if (m_rua.v_ctl[0] != '\0')
	{
		if (m_VideoCodec == VIDEO_CODEC_H264)
		{
			h264_rxi_init(&h264rxi, video_data_cb, this);
		}
		else if (m_VideoCodec == VIDEO_CODEC_H265)
		{
			h265_rxi_init(&h265rxi, video_data_cb, this);
		}
		else if (m_VideoCodec == VIDEO_CODEC_JPEG)
		{
			mjpeg_rxi_init(&mjpegrxi, video_data_cb, this);
		}
		else if (m_VideoCodec == VIDEO_CODEC_MP4)
		{
			mpeg4_rxi_init(&mpeg4rxi, video_data_cb, this);
		}
	}

    if (m_rua.a_ctl[0] != '\0')
    {
		if (m_AudioCodec == AUDIO_CODEC_AAC)
		{
			aac_rxi_init(&aacrxi, audio_data_cb, this);
		}
		else if (AUDIO_CODEC_NONE != m_AudioCodec)
		{
			pcm_rxi_init(&pcmrxi, audio_data_cb, this);
		}
    }

	if (!m_rua.rtp_tcp)
	{
    	m_udpRxTid = sys_os_create_thread((void *)rtsp_udp_rx_thread, this);
    }

    return TRUE;
}

void CRtsp::tcp_data_rx(uint8 * lpData, int rlen)
{
	RILF * p_rilf = (RILF *)lpData;
	uint8 * p_rtp = (uint8 *)p_rilf + 4;
	uint32 rtp_len = rlen - 4;

	if (rtp_len >= 2 && RTP_PT_IS_RTCP(p_rtp[1])) // now, don't handle rtcp packet ...
	{
		return;
	}

	if (p_rilf->channel == m_rua.v_interleaved)
	{
		if (VIDEO_CODEC_H264 == m_VideoCodec)
		{
			h264_rtp_rx(&h264rxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_JPEG == m_VideoCodec)
		{
			mjpeg_rtp_rx(&mjpegrxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_MP4 == m_VideoCodec)
		{
			mpeg4_rtp_rx(&mpeg4rxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_H265 == m_VideoCodec)
		{
			h265_rtp_rx(&h265rxi, p_rtp, rtp_len);
		}
	}
	else if (p_rilf->channel == m_rua.a_interleaved)
	{
		if (AUDIO_CODEC_AAC == m_AudioCodec)
		{
			aac_rtp_rx(&aacrxi, p_rtp, rtp_len);
		}
		else if (AUDIO_CODEC_NONE != m_AudioCodec)
		{
			pcm_rtp_rx(&pcmrxi, p_rtp, rtp_len);
		}
	}
#ifdef METADATA
	else if (p_rilf->channel == m_rua.m_interleaved)
	{
		if (rtp_data_rx(&rtprxi, p_rtp, rtp_len))
		{
			metadata_rtp_rx(rtprxi.p_data, rtprxi.len, rtprxi.prev_ts, rtprxi.prev_seq);
		}
	}
#endif
}

void CRtsp::udp_data_rx(uint8 * lpData, int rlen, int type)
{
	uint8 * p_rtp = lpData;
	uint32 rtp_len = rlen;

	if (rtp_len >= 2 && RTP_PT_IS_RTCP(p_rtp[1])) // now, don't handle rtcp packet ...
	{
		return;
	}

	if (AV_TYPE_VIDEO == type)
	{
		if (VIDEO_CODEC_H264 == m_VideoCodec)
		{
			h264_rtp_rx(&h264rxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_JPEG == m_VideoCodec)
		{
			mjpeg_rtp_rx(&mjpegrxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_MP4 == m_VideoCodec)
		{
			mpeg4_rtp_rx(&mpeg4rxi, p_rtp, rtp_len);
		}
		else if (VIDEO_CODEC_H265 == m_VideoCodec)
		{
			h265_rtp_rx(&h265rxi, p_rtp, rtp_len);
		}
	}
	else if (AV_TYPE_AUDIO == type)
	{
		if (AUDIO_CODEC_AAC == m_AudioCodec)
		{
			aac_rtp_rx(&aacrxi, p_rtp, rtp_len);
		}
		else if (AUDIO_CODEC_NONE != m_AudioCodec)
		{
			pcm_rtp_rx(&pcmrxi, p_rtp, rtp_len);
		}
	}
#ifdef METADATA
	else if (AV_TYPE_METADATA == type)
	{
		if (rtp_data_rx(&rtprxi, p_rtp, rtp_len))
		{
			metadata_rtp_rx(rtprxi.p_data, rtprxi.len, rtprxi.prev_ts, rtprxi.prev_seq);
		}
	}
#endif
}

// rtsp_tcp_rx() 함수 내, rx_point 레이블 바로 위에 추가할 재동기화 함수
// 반환값: 버퍼 내 유효한 패킷 시작점의 오프셋. 못 찾으면 -1.
int find_valid_packet_offset(const char* buf, int len)
{
    for (int i = 0; i < len; ++i)
    {
        // RTP Interleaved 패킷 시작('$') 확인
        if (buf[i] == 0x24)
        {
            // '$' 뒤에 채널 ID와 길이가 와야 하므로 최소 4바이트가 남아있는지 확인
            if (len - i >= 4) return i;
        }
        // RTSP 메시지 시작("RTSP") 확인
        if (len - i >= 4 && strncmp(buf + i, "RTSP", 4) == 0)
        {
            return i;
        }
    }
    return -1; // 유효한 시작점을 찾지 못함
}

int CRtsp::rtsp_msg_parser(RCUA * p_rua)
{
	//int rtsp_pkt_len = rtsp_pkt_find_end(p_rua->rcv_buf);
	int rtsp_pkt_len = rtsp_pkt_find_end_len(p_rua->rcv_buf, p_rua->rcv_dlen);
	if (rtsp_pkt_len == 0) // wait for next recv
	{
		return RTSP_PARSE_MOREDATE;
	}

	HRTSP_MSG * rx_msg = rtsp_get_msg_buf();
	if (rx_msg == NULL)
	{
		log_print(LOG_ERR, "%s, rtsp_get_msg_buf return null!!!\r\n", __FUNCTION__);
		return RTSP_PARSE_FAIL;
	}

	memcpy(rx_msg->msg_buf, p_rua->rcv_buf, rtsp_pkt_len);
	rx_msg->msg_buf[rtsp_pkt_len] = '\0';

	log_print(LOG_DBG, "RX << %s\r\n", rx_msg->msg_buf);

	int parse_len = rtsp_msg_parse_part1(rx_msg->msg_buf, rtsp_pkt_len, rx_msg);
	if (parse_len != rtsp_pkt_len)	//parse error
	{
		log_print(LOG_ERR, "%s, rtsp_msg_parse_part1=%d, rtsp_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, rtsp_pkt_len);
		rtsp_free_msg(rx_msg);

		p_rua->rcv_dlen = 0;
		return RTSP_PARSE_FAIL;
	}

	if (rx_msg->ctx_len > 0)
	{
		if (p_rua->rcv_dlen < (parse_len + rx_msg->ctx_len))
		{
			rtsp_free_msg(rx_msg);
			return RTSP_PARSE_MOREDATE;
		}

		memcpy(rx_msg->msg_buf+rtsp_pkt_len, p_rua->rcv_buf+rtsp_pkt_len, rx_msg->ctx_len);
        rx_msg->msg_buf[rtsp_pkt_len+rx_msg->ctx_len] = '\0';

        log_print(LOG_DBG, "%s\r\n", rx_msg->msg_buf+rtsp_pkt_len);

		int sdp_parse_len = rtsp_msg_parse_part2(rx_msg->msg_buf+rtsp_pkt_len, rx_msg->ctx_len, rx_msg);
		if (sdp_parse_len != rx_msg->ctx_len)
		{
		}
		parse_len += rx_msg->ctx_len;
	}

	if (parse_len < p_rua->rcv_dlen)
	{
		while (p_rua->rcv_buf[parse_len] == ' ' || p_rua->rcv_buf[parse_len] == '\r' || p_rua->rcv_buf[parse_len] == '\n')
		{
			parse_len++;
		}

		memmove(p_rua->rcv_buf, p_rua->rcv_buf + parse_len, p_rua->rcv_dlen - parse_len);
		p_rua->rcv_dlen -= parse_len;
	}
	else
	{
		p_rua->rcv_dlen = 0;
	}

	int ret = rtsp_client_state(p_rua, rx_msg);

	rtsp_free_msg(rx_msg);

	return ret ? RTSP_PARSE_SUCC : RTSP_PARSE_FAIL;
}

int CRtsp::rtsp_tcp_rx()
{
	RCUA * p_rua = &(this->m_rua);

	if (p_rua->fd <= 0)
	{
		return -1;
	}

    int sret;
	int delayUsec = 100 * 1000;
    fd_set fdread;
    struct timeval tv = { 0, delayUsec };

    FD_ZERO(&fdread);
    FD_SET(p_rua->fd, &fdread);

    sret = select((int)(p_rua->fd+1), &fdread, NULL, NULL, &tv);
    if (sret == 0) // Time expired
    {
#if 0
		if( p_rua && p_rua->uri )
			printf( "[Timeout<%d>] URI : %s\n", p_rua->fd, p_rua->uri );
#endif
        return RTSP_RX_TIMEOUT;
    }
    else if (!FD_ISSET(p_rua->fd, &fdread))
    {
        return RTSP_RX_TIMEOUT;
    }

	if( m_rtsp_rtx_recv_count++ > 210'000'000 )
		m_rtsp_rtx_recv_count = 0;

	if (p_rua->rtp_rcv_buf == NULL || p_rua->rtp_t_len == 0)
	{
		int rlen = recv(p_rua->fd, p_rua->rcv_buf+p_rua->rcv_dlen, 2048-p_rua->rcv_dlen, 0);
		if (rlen <= 0)
		{
			int error_code = errno;

			// log_print(H_LOG_WARN, "%s, thread exit, ret = %d, err = %s\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());	//recv error, connection maybe disconn?
			// MLOG_WARN("RTSP tcp recv failed ( rlen = %d ) [1] { %s } : %s ( errno = %d ) ( rcv_cnt = %d )", rlen, p_rua->uri, strerror(error_code), error_code, m_rtsp_rtx_recv_count );

			#if 1
			switch( error_code )
			{
				// 재시도가 가능한 경우
				case EINTR:
				case EAGAIN:
				// case EWOULDBLOCK: == EAGAIN
					return RTSP_RX_SUCC;

				// 복구 불가능한 오류 (그 외 모든 경우)
				default:
					return RTSP_RX_FAIL;
			}
			#else
			return RTSP_RX_FAIL;
			#endif
		}

		p_rua->rcv_dlen += rlen;

		if (p_rua->rcv_dlen < 4)
		{
			return RTSP_RX_SUCC;
		}
	}
	else
	{
		int rlen = recv(p_rua->fd, p_rua->rtp_rcv_buf+p_rua->rtp_rcv_len, p_rua->rtp_t_len-p_rua->rtp_rcv_len, 0);
		if (rlen <= 0)
		{
			int error_code = errno;

			// log_print(H_LOG_WARN, "%s, thread exit��ret = %d, err = %s\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());	//recv error, connection maybe disconn?
			// MLOG_WARN("RTSP tcp recv failed ( rlen = %d ) [2] { %s } : %s ( errno = %d ) ( rcv_cnt = %d )", rlen, p_rua->uri, strerror(error_code), error_code, m_rtsp_rtx_recv_count );

			#if 1
			switch( error_code )
			{
				// 재시도가 가능한 경우
				case EINTR:
				case EAGAIN:
				// case EWOULDBLOCK: == EAGAIN
					return RTSP_RX_SUCC;

				// 복구 불가능한 오류 (그 외 모든 경우)
				default:
					return RTSP_RX_FAIL;
			}
			#else
			return RTSP_RX_FAIL;
			#endif
		}

		p_rua->rtp_rcv_len += rlen;
		if (p_rua->rtp_rcv_len == p_rua->rtp_t_len)
		{
			tcp_data_rx((uint8*)p_rua->rtp_rcv_buf, p_rua->rtp_rcv_len);

			free(p_rua->rtp_rcv_buf);
			p_rua->rtp_rcv_buf = NULL;
			p_rua->rtp_rcv_len = 0;
			p_rua->rtp_t_len = 0;
		}

		return RTSP_RX_SUCC;
	}

rx_point:
	// 선행 공백 문자 제거 로직
	int skip_len = 0;
	while (skip_len < p_rua->rcv_dlen) {
		char c = p_rua->rcv_buf[skip_len];
		if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
			skip_len++;
		} else {
			break;
		}
	}

	if (skip_len > 0) {
		memmove(p_rua->rcv_buf, p_rua->rcv_buf + skip_len, p_rua->rcv_dlen - skip_len);
		p_rua->rcv_dlen -= skip_len;
	}

    if (p_rua->rcv_dlen < 4) // 최소 패킷 길이를 만족하지 못하면 일단 더 받음
    {
        return RTSP_RX_SUCC;
    }

	if (rtsp_is_rtsp_msg(p_rua->rcv_buf))	//Is RTSP Packet?
	{
		int ret = rtsp_msg_parser(p_rua);
		if (ret == RTSP_PARSE_FAIL)
		{
			// MLOG_WARN("(RTSP_RX_FAIL) RTSP msg parse issue { %s } ( rcv_count = %d )", p_rua->uri, m_rtsp_rtx_recv_count );

			// 파서가 실패했으므로, 버퍼의 맨 처음부터 다시 유효한 시작점을 찾는다.
			int offset = find_valid_packet_offset(p_rua->rcv_buf, p_rua->rcv_dlen);

			if (offset == 0)
			{
				// **무한 루프 방지**
				// 유효한 시작점이 현재 위치(0)라고 나왔지만 파싱에 실패했다는 것은
				// 이 시작점 자체가 잘못되었다는 의미.
				// 따라서 최소 1바이트를 버리고 다음을 기약한다.
				// MLOG_WARN("Resync: Found problematic packet start at offset 0. Discarding 1 byte to prevent loop.");
				p_rua->rcv_dlen -= 1;
				memmove(p_rua->rcv_buf, p_rua->rcv_buf + 1, p_rua->rcv_dlen);
				goto rx_point;
			}
			else if (offset > 0)
			{
				// 버퍼 중간에서 유효한 시작점을 찾았다. 그 앞부분의 쓰레기 데이터를 버린다.
				// MLOG_INFO("Resync stream: Found valid packet start at offset %d. Discarding garbage data.", offset);
				p_rua->rcv_dlen -= offset;
				memmove(p_rua->rcv_buf, p_rua->rcv_buf + offset, p_rua->rcv_dlen);
				goto rx_point; // 파싱 재시도
			}
			else // offset == -1
			{
				// MLOG_WARN("Resync failed. Preserving last 3 bytes for boundary recovery.");
				if (p_rua->rcv_dlen > 3) {
					// 뒤의 3바이트만 앞으로 남겨서 다음 수신과 이어붙임
					memmove(p_rua->rcv_buf, p_rua->rcv_buf + (p_rua->rcv_dlen - 3), 3);
					p_rua->rcv_dlen = 3;
				}
				// 원래 0~3바이트면 그대로 유지됨

				// 이 경우, 다음 recv()에서 데이터를 새로 받는 것이 최선이므로 SUCC를 반환한다.
				// 하지만 반복적으로 이 로그가 발생한다면 근본적인 스트림 손상 문제일 수 있다.
				return RTSP_RX_SUCC;
			}
		}
		else if (ret == RTSP_PARSE_MOREDATE)
		{
			return RTSP_RX_SUCC;
		}

		if (p_rua->rcv_dlen >= 4)
		{
			goto rx_point;
		}
	}
	else
	{
		RILF * p_rilf = (RILF *)(p_rua->rcv_buf);
		if (p_rilf->magic != 0x24)
		{
			// log_print(H_LOG_WARN, "%s, p_rilf->magic[0x%02X]!!!\r\n", __FUNCTION__, p_rilf->magic);
			// MLOG_WARN("RTSP recv magic number 0x24, but current magic number is [%02X] in { %s } ( rcv_count = %d )", p_rilf->magic, p_rua->uri, m_rtsp_rtx_recv_count );

            // --- 재동기화 로직 추가 ---
            int offset = find_valid_packet_offset(p_rua->rcv_buf, p_rua->rcv_dlen);
            if (offset > 0) // 0보다 커야 의미 있음 (0이면 원래 로직에서 처리됨)
            {
                // MLOG_INFO("Resync stream: Discarding %d bytes of garbage data.", offset);
                p_rua->rcv_dlen -= offset;
                memmove(p_rua->rcv_buf, p_rua->rcv_buf + offset, p_rua->rcv_dlen);
                goto rx_point; // 파싱 재시도
            }
            else
            {
                // MLOG_WARN("Resync failed. Discarding whole buffer.");
                p_rua->rcv_dlen = 0; // 버퍼 전체를 비우고 다시 시작
                return RTSP_RX_FAIL; // 이 경우는 심각한 오류이므로 연결 종료
            }
		}

		uint16 rtp_len = ntohs(p_rilf->rtp_len);

		// rtp_len이 최소 12바이트(RTP 헤더)보다 작거나, 비정상적으로 큰 값(e.g. 4096)일 경우
		if (rtp_len < 12 || rtp_len > 4096) {
			MLOG_WARN("Invalid interleaved length=%u; discard 1 byte & resync", rtp_len);
			// 버퍼 1바이트 버리고 재동기화
			p_rua->rcv_dlen -= 1;
			memmove(p_rua->rcv_buf, p_rua->rcv_buf + 1, p_rua->rcv_dlen);
			goto rx_point;
		}

		if (rtp_len > (p_rua->rcv_dlen - 4))
		{
			if (p_rua->rtp_rcv_buf)
			{
				free(p_rua->rtp_rcv_buf);
			}

			p_rua->rtp_rcv_buf = (char *)malloc(rtp_len+4);
			if (p_rua->rtp_rcv_buf == NULL)
			{
				// MLOG_WARN("(RTSP_RX_FAIL) = CASE 6 = { %s } >> %s", p_rua->uri, sys_os_get_socket_error() );
			    return RTSP_RX_FAIL;
			}

			memcpy(p_rua->rtp_rcv_buf, p_rua->rcv_buf, p_rua->rcv_dlen);
			p_rua->rtp_rcv_len = p_rua->rcv_dlen;
			p_rua->rtp_t_len = rtp_len+4;

			p_rua->rcv_dlen = 0;

			return RTSP_RX_SUCC;
		}

		tcp_data_rx((uint8*)p_rilf, rtp_len+4);

		p_rua->rcv_dlen -= rtp_len+4;
		if (p_rua->rcv_dlen > 0)
		{
			memmove(p_rua->rcv_buf, p_rua->rcv_buf+rtp_len+4, p_rua->rcv_dlen);
		}

		if (p_rua->rcv_dlen >= 4)
		{
			goto rx_point;
		}
	}

	return RTSP_RX_SUCC;
}

int CRtsp::rtsp_udp_rx()
{
    fd_set fdr;
	int max_fd = 0;

	FD_ZERO(&fdr);

    if (m_rua.v_udp_fd)
    {
        FD_SET(m_rua.v_udp_fd, &fdr);
        max_fd = (max_fd >= (int)m_rua.v_udp_fd)? max_fd : (int)m_rua.v_udp_fd;
    }

    if (m_rua.a_udp_fd)
    {
        FD_SET(m_rua.a_udp_fd, &fdr);
        max_fd = (max_fd >= (int)m_rua.a_udp_fd)? max_fd : (int)m_rua.a_udp_fd;
    }

#ifdef METADATA
    if (m_rua.m_udp_fd)
    {
        FD_SET(m_rua.m_udp_fd, &fdr);
        max_fd = (max_fd >= (int)m_rua.m_udp_fd)? max_fd : (int)m_rua.m_udp_fd;
    }
#endif

	struct timeval tv = {0, 100*1000};

	int sret = select(max_fd+1, &fdr, NULL, NULL, &tv);
	if (sret <= 0)
	{
		return RTSP_RX_TIMEOUT;
    }

    int alen;
    char vbuf[2048];
    struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	alen = sizeof(struct sockaddr_in);

    if (m_rua.v_udp_fd && FD_ISSET(m_rua.v_udp_fd, &fdr))
    {
        int rlen = recvfrom(m_rua.v_udp_fd, vbuf, sizeof(vbuf), 0, (struct sockaddr *)&addr, (socklen_t*)&alen);
    	if (rlen <= 12)
    	{
    		log_print(LOG_ERR, "%s, recvfrom return %d, err[%s]!!!\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());
    	}
    	else
    	{
    	    udp_data_rx((uint8*)vbuf, rlen, AV_TYPE_VIDEO);
    	}
    }

    memset(&addr, 0, sizeof(addr));
	alen = sizeof(struct sockaddr_in);

    if (m_rua.a_udp_fd && FD_ISSET(m_rua.a_udp_fd, &fdr))
    {
        int rlen = recvfrom(m_rua.a_udp_fd, vbuf, sizeof(vbuf), 0, (struct sockaddr *)&addr, (socklen_t*)&alen);
    	if (rlen <= 12)
    	{
    		log_print(LOG_ERR, "%s, recvfrom return %d, err[%s]!!!\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());
    	}
    	else
    	{
    	    udp_data_rx((uint8*)vbuf, rlen, AV_TYPE_AUDIO);
    	}
    }

#ifdef METADATA

	memset(&addr, 0, sizeof(addr));
	alen = sizeof(struct sockaddr_in);

    if (m_rua.m_udp_fd && FD_ISSET(m_rua.m_udp_fd, &fdr))
    {
        int rlen = recvfrom(m_rua.m_udp_fd, vbuf, sizeof(vbuf), 0, (struct sockaddr *)&addr, (socklen_t*)&alen);
    	if (rlen <= 12)
    	{
    		log_print(LOG_ERR, "%s, recvfrom return %d, err[%s]!!!\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());
    	}
    	else
    	{
    	    udp_data_rx((uint8*)vbuf, rlen, AV_TYPE_METADATA);
    	}
    }

#endif

    return RTSP_RX_SUCC;
}

// #include "MgenLogger.h"
BOOL CRtsp::rtsp_start(const char * url, const char * ip, int port, const char * user, const char * pass)
{
	if (m_rua.state != RCS_NULL)
	{
		rtsp_play();
		return TRUE;
	}

	if (user && user != m_rua.auth_info.auth_name)
	{
		strcpy(m_rua.auth_info.auth_name, user);
	}

	if (pass && pass != m_rua.auth_info.auth_pwd)
	{
		strcpy(m_rua.auth_info.auth_pwd, pass);
	}

	if (url && url != m_url)
	{
		strcpy(m_url, url);
	}

	if (ip && ip != m_ip)
	{
		strcpy(m_ip, ip);
	}

	m_nport = port;

	// MLOG_INFO("{ %s } RTSP START first", url); << 껐다 킬 때마다 얘 탐
    m_bRunning = TRUE;
	m_tcpRxTid = sys_os_create_thread((void *)rtsp_tcp_rx_thread, this);
	if (m_tcpRxTid == 0)
	{
		log_print(LOG_ERR, "%s, sys_os_create_thread failed!!!\r\n", __FUNCTION__);
		return FALSE;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_start(const char * url, char * user, char * pass)
{
    char* username = NULL;
	char* password = NULL;
	char* address = NULL;
	int   urlPortNum = 554;

	if (!parse_url(url, username, password, address, urlPortNum, NULL))
	{
		return FALSE;
	}

	if (username)
	{
	    strcpy(m_rua.auth_info.auth_name, username);
		delete[] username;
		username = m_rua.auth_info.auth_name;
	}
	else
	{
	    username = user;
	}

	if (password)
	{
		strcpy(m_rua.auth_info.auth_pwd, password);
		delete[] password;
		password = m_rua.auth_info.auth_pwd;
	}
	else
	{
	    password = pass;
	}
	strncpy(m_ip, address, sizeof(m_ip) - 1);
	delete[] address;

	m_nport = urlPortNum;
	//printf("url: %s, ip: %s\n", url, m_ip);

    return rtsp_start(url, m_ip, m_nport, username, password);
}

void CRtsp::copy_str_from_url(char* dest, char const* src, unsigned len)
{
	// Normally, we just copy from the source to the destination.  However, if the source contains
	// %-encoded characters, then we decode them while doing the copy:
	while (len > 0)
	{
		int nBefore = 0;
		int nAfter = 0;

		if (*src == '%' && len >= 3 && sscanf(src+1, "%n%2hhx%n", &nBefore, dest, &nAfter) == 1)
		{
			unsigned codeSize = nAfter - nBefore; // should be 1 or 2

			++dest;
			src += (1 + codeSize);
			len -= (1 + codeSize);
		}
		else
		{
			*dest++ = *src++;
			--len;
		}
	}

	*dest = '\0';
}

BOOL CRtsp::parse_url(char const* url, char*& username, char*& password, char*& address, int& portNum, char const** urlSuffix)
{
	do
	{
		// Parse the URL as "rtsp://[<username>[:<password>]@]<server-address-or-name>[:<port>][/<stream-name>]"
		char const* prefix = "rtsp://";
		unsigned const prefixLength = 7;
		if (strnicmp(url, prefix, prefixLength) != 0)
		{
			log_print(LOG_ERR, "%s, URL is not of the form \"%s\"\r\n", __FUNCTION__, prefix);
			break;
		}

		unsigned const parseBufferSize = 100;
		char parseBuffer[parseBufferSize];
		char const* from = &url[prefixLength];

		// Check whether "<username>[:<password>]@" occurs next.
		// We do this by checking whether '@' appears before the end of the URL, or before the first '/'.
		username = password = address = NULL; // default return values
		char const* colonPasswordStart = NULL;
		char const* p;

		for (p = from; *p != '\0' && *p != '/'; ++p)
		{
			if (*p == ':' && colonPasswordStart == NULL)
			{
				colonPasswordStart = p;
			}
			else if (*p == '@')
			{
				// We found <username> (and perhaps <password>).  Copy them into newly-allocated result strings:
				if (colonPasswordStart == NULL)
				{
					colonPasswordStart = p;
				}

				char const* usernameStart = from;
				unsigned usernameLen = (unsigned)(colonPasswordStart - usernameStart);
				username = new char[usernameLen + 1] ; // allow for the trailing '\0'
				copy_str_from_url(username, usernameStart, usernameLen);

				char const* passwordStart = colonPasswordStart;
				if (passwordStart < p) ++passwordStart; // skip over the ':'
				unsigned passwordLen = (unsigned)(p - passwordStart);
				password = new char[passwordLen + 1]; // allow for the trailing '\0'
				copy_str_from_url(password, passwordStart, passwordLen);

				from = p + 1; // skip over the '@'
				break;
			}
		}

		// Next, parse <server-address-or-name>
		char* to = &parseBuffer[0];
		unsigned i;

		for (i = 0; i < parseBufferSize; ++i)
		{
			if (*from == '\0' || *from == ':' || *from == '/')
			{
				// We've completed parsing the address
				*to = '\0';
				break;
			}
			*to++ = *from++;
		}

		if (i == parseBufferSize)
		{
			log_print(LOG_ERR, "%s, URL is too long\r\n", __FUNCTION__);
			break;
		}

		address = new char[strlen(parseBuffer)+1] ;
		strcpy(address, parseBuffer);

		portNum = 554; // default value
		char nextChar = *from;
		if (nextChar == ':')
		{
			int portNumInt;
			if (sscanf(++from, "%d", &portNumInt) != 1)
			{
				log_print(LOG_ERR, "%s, No port number follows ':'\r\n", __FUNCTION__);
				break;
			}

			if (portNumInt < 1 || portNumInt > 65535)
			{
				log_print(LOG_ERR, "%s, Bad port number\r\n", __FUNCTION__);
				break;
			}

			portNum = portNumInt;

			while (*from >= '0' && *from <= '9')
			{
				++from; // skip over port number
			}
		}

		// The remainder of the URL is the suffix:
		if (urlSuffix != NULL)
		{
			*urlSuffix = from;
		}

		return TRUE;
	} while (0);

  	return FALSE;
}

BOOL CRtsp::rtsp_play()
{
	m_rua.cseq++;

	HRTSP_MSG * tx_msg = rua_build_play(&m_rua);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(&m_rua, tx_msg);
	}

	return TRUE;
}

BOOL CRtsp::rtsp_stop()
{
	int ruaState = 0;
	sys_os_mutex_enter(ruaStateMutex);
	ruaState = m_rua.state;
	sys_os_mutex_leave(ruaStateMutex);

    if (RCS_NULL == ruaState)
    {
        return TRUE;
    }

	m_rua.cseq++;

	HRTSP_MSG * tx_msg = rua_build_teardown(&m_rua);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(&m_rua, tx_msg);
	}

	sys_os_mutex_enter(ruaStateMutex);
	m_rua.state = RCS_NULL;
	sys_os_mutex_leave(ruaStateMutex);

	return TRUE;
}

BOOL CRtsp::rtsp_pause()
{
	m_rua.cseq++;

	HRTSP_MSG * tx_msg = rua_build_pause(&m_rua);
	if (tx_msg)
	{
		rcua_send_free_rtsp_msg(&m_rua, tx_msg);
	}

	return TRUE;
}

BOOL CRtsp::rtsp_close()
{
	sys_os_mutex_enter(m_pMutex);
	m_pAudioCB = NULL;
	m_pVideoCB = NULL;
#ifdef METADATA
	m_pMetadataCB = NULL;
#endif
	m_pNotify = NULL;
	m_pUserdata = NULL;
	m_pRedirectCB = NULL;

	sys_os_mutex_leave(m_pMutex);

    m_bRunning = FALSE;
	while (m_tcpRxTid != 0)
	{
		usleep(10*1000);
	}

	while (m_udpRxTid != 0)
	{
		usleep(10*1000);
	}

	if (m_rua.v_udp_fd > 0)
	{
		closesocket(m_rua.v_udp_fd);
		m_rua.v_udp_fd = 0;
	}

	if (m_rua.a_udp_fd > 0)
	{
		closesocket(m_rua.a_udp_fd);
		m_rua.a_udp_fd = 0;
	}

#ifdef METADATA
	if (m_rua.m_udp_fd > 0)
	{
		closesocket(m_rua.m_udp_fd);
		m_rua.m_udp_fd = 0;
	}
#endif

#ifdef BACKCHANNEL
	if (m_rua.bc_udp_fd > 0)
	{
		closesocket(m_rua.bc_udp_fd);
		m_rua.bc_udp_fd = 0;
	}

#if __WINDOWS_OS__
	if (m_rua.audio_captrue)
	{
		m_rua.audio_captrue->delCallback(rtsp_bc_cb, &m_rua);
		m_rua.audio_captrue->freeInstance(0);
		m_rua.audio_captrue = NULL;
	}
#elif defined(ANDROID)
    if (m_rua.audio_input)
    {
        delete m_rua.audio_input;
		m_rua.audio_input = NULL;
    }
#endif
#endif

	if (m_VideoCodec == VIDEO_CODEC_H264)
	{
		h264_rxi_deinit(&h264rxi);
	}
	else if (m_VideoCodec == VIDEO_CODEC_H265)
	{
		h265_rxi_deinit(&h265rxi);
	}
	else if (m_VideoCodec == VIDEO_CODEC_JPEG)
	{
		mjpeg_rxi_deinit(&mjpegrxi);
	}
	else if (m_VideoCodec == VIDEO_CODEC_MP4)
	{
		mpeg4_rxi_deinit(&mpeg4rxi);
	}

	if (m_AudioCodec == AUDIO_CODEC_AAC)
	{
		aac_rxi_deinit(&aacrxi);
	}
	else if (m_AudioCodec != AUDIO_CODEC_NONE)
	{
		pcm_rxi_deinit(&pcmrxi);
	}

	memset(&rtprxi, 0, sizeof(RTPRXI));

	if (m_pAudioConfig)
	{
		delete [] m_pAudioConfig;
		m_pAudioConfig = NULL;
	}

    memset(&m_rua, 0, sizeof(RCUA));
    memset(&m_url, 0, sizeof(m_url));
    memset(&m_ip, 0, sizeof(m_ip));
    m_nport = 554;

    m_rua.session_timeout = 60;

    m_pAudioConfig = NULL;
    m_nAudioConfigLen = 0;

    m_VideoCodec = VIDEO_CODEC_NONE;
    m_AudioCodec = AUDIO_CODEC_NONE;
    m_nSamplerate = 0;
    m_nChannels = 0;

#ifdef BACKCHANNEL
    m_bcAudioCodec = AUDIO_CODEC_NONE;
    m_nbcSamplerate = 0;
    m_nbcChannels = 0;
#endif

	return TRUE;
}

int CRtsp::get_rua_state()
{
	int ruaState = 0;
	int connectionState = 5;

#if 0
	sys_os_mutex_enter(ruaStateMutex);
#else
	if ( sys_os_sig_wait_timeout( ruaStateMutex, 10 ) == -1 ) {
		printf( "get_rua_state mtx timeout\n" );
		return connectionState;
	}
#endif
	ruaState = m_rua.state;
	sys_os_mutex_leave(ruaStateMutex);

	switch (ruaState)
	{
	case RCS_NULL:
		//NULL
		connectionState = 5;
		break;

	case RCS_OPTIONS:
		//OPTIONS
		connectionState = 0;
		break;

	case RCS_DESCRIBE:
		//DISCRIBE
		connectionState = 1;
		break;

	case RCS_READY:
		//SETUP
		connectionState = 2;
		break;

	case RCS_PLAYING:
		//PLAY
		connectionState = 3;
		break;
	default:
		//Unhandled cases
		connectionState = 5;
		break;
	}
	return connectionState;
}

void CRtsp::rtsp_video_data_cb(uint8 * p_data, int len, uint32 ts, uint32 seq)
{
	sys_os_mutex_enter(m_pMutex);
	if (m_pVideoCB)
	{
		//rtsp_video_cb
		m_pVideoCB(p_data, len, ts, seq, m_pUserdata); // Here Decode
	}
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::rtsp_audio_data_cb(uint8 * p_data, int len, uint32 ts, uint32 seq)
{
	sys_os_mutex_enter(m_pMutex);
	if (m_pAudioCB)
	{
		m_pAudioCB(p_data, len, ts, seq, m_pUserdata);
	}
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::rtsp_keep_alive()
{
	uint32 ms = sys_os_get_ms();
	if (ms - m_rua.keepalive_time >= (uint32)(m_rua.session_timeout - 10) * 1000)
	{
		m_rua.keepalive_time = ms;

		m_rua.cseq++;

		HRTSP_MSG * tx_msg;

		if (m_rua.gp_cmd) // the rtsp server supports GET_PARAMETER command
		{
			tx_msg = rua_build_get_parameter(&m_rua);
		}
		else
		{
			tx_msg = rua_build_options(&m_rua);
		}

		if (tx_msg)
    	{
    		rcua_send_free_rtsp_msg(&m_rua, tx_msg);
    	}
	}
}

void CRtsp::tcp_rx_thread()
{
	int  ret;
	int  tm_count = 0;
    BOOL nodata_notify = FALSE;

	send_notify(RTSP_EVE_CONNECTING);

	if (!rtsp_client_start())
	{
		send_notify(RTSP_EVE_CONNFAIL);
		goto rtsp_rx_exit;
	}

	while (m_bRunning)
	{
	    ret = rtsp_tcp_rx();
	    if (ret == RTSP_RX_FAIL)
	    {
			// MLOG_WARN("<RTSP_RX_FAIL> from rtsp_tcp_rx() { %s }", m_rua.uri );
	        break;
	    }
	    else if (m_rua.rtp_tcp)
	    {
		    if (ret == RTSP_RX_TIMEOUT)
	    	{
	    		tm_count++;
		        if (tm_count >= 100 && !nodata_notify)    // in 10s without data
		        {
					// MLOG_WARN( "[Timeout] Over 10 sec without data ( %.2f sec ) { %s }", tm_count / 10.0f , m_rua.uri );
		            nodata_notify = TRUE;
		            send_notify(RTSP_EVE_NODATA);
		        }
	        }
	        else // should be RTSP_RX_SUCC
	        {
	        	if (nodata_notify)
		        {
		            nodata_notify = FALSE;
		            send_notify(RTSP_EVE_RESUME);
		        }

	        	tm_count = 0;
	        }
        }
        else
        {
        	if (ret == RTSP_RX_TIMEOUT)
        	{
        		usleep(100*1000);
        	}
        }
		int ruaState = 0;
		sys_os_mutex_enter(ruaStateMutex);
		ruaState = m_rua.state;
		sys_os_mutex_leave(ruaStateMutex);

	    if (ruaState == RCS_PLAYING)
	    {
	    	rtsp_keep_alive();
	    }
	}

    if (m_rua.fd > 0)
	{
		closesocket(m_rua.fd);
		m_rua.fd = 0;
	}

    if (m_rua.rtp_rcv_buf)
    {
        free(m_rua.rtp_rcv_buf);
        m_rua.rtp_rcv_buf = NULL;
    }

	// MLOG_WARN("{ RTSP_EVE_STOPPED } Event Sended { %s }", m_rua.uri );
	send_notify(RTSP_EVE_STOPPED);

rtsp_rx_exit:

	m_tcpRxTid = 0;
	log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);
}

void CRtsp::udp_rx_thread()
{
	int  ret;
	int  tm_count = 0;
    BOOL nodata_notify = FALSE;

    while (m_bRunning)
	{
	    ret = rtsp_udp_rx();
	    if (ret == RTSP_RX_FAIL)
	    {
	        break;
	    }
	    else if (ret == RTSP_RX_TIMEOUT)
    	{
    		tm_count++;
	        if (tm_count >= 100 && !nodata_notify)    // in 10s without data
	        {
	            nodata_notify = TRUE;
	            send_notify(RTSP_EVE_NODATA);
	        }
        }
        else // should be RTSP_RX_SUCC
        {
        	if (nodata_notify)
	        {
	            nodata_notify = FALSE;
	            send_notify(RTSP_EVE_RESUME);
	        }

        	tm_count = 0;
        }
	}

	m_udpRxTid = 0;

	log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);
}

void CRtsp::send_notify(int event)
{
	sys_os_mutex_enter(m_pMutex);

	if (m_pNotify)
	{
		m_pNotify(event, m_pUserdata);
	}

	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::send_redirect_info(char *url)
{
	sys_os_mutex_enter(m_pMutex);

	if (m_pRedirectCB)
	{
		m_pRedirectCB(url, m_pUserdata);
	}

	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::get_h264_params()
{
	rtsp_send_h264_params(&m_rua);
}

BOOL CRtsp::get_h264_params(uint8 * p_sps, int * sps_len, uint8 * p_pps, int * pps_len)
{
	char sps[1000], pps[1000] = {'\0'};

	if (!rua_get_sdp_h264_params(&m_rua, NULL, sps, sizeof(sps)))
	{
		return FALSE;
	}

	char * ptr = strchr(sps, ',');
	if (ptr && ptr[1] != '\0')
	{
		*ptr = '\0';
		strcpy(pps, ptr+1);
	}

	uint8 sps_pps[1000];
	sps_pps[0] = 0x0;
	sps_pps[1] = 0x0;
	sps_pps[2] = 0x0;
	sps_pps[3] = 0x1;

	int len = base64_decode(sps, sps_pps+4, sizeof(sps_pps)-4);
	if (len <= 0)
	{
		return FALSE;
	}

    if ((sps_pps[4] & 0x1f) == 7)
    {
	    memcpy(p_sps, sps_pps, len+4);
	    *sps_len = len+4;
	}
	else if ((sps_pps[4] & 0x1f) == 8)
	{
	    memcpy(p_pps, sps_pps, len+4);
		*pps_len = len+4;
	}

	if (pps[0] != '\0')
	{
		len = base64_decode(pps, sps_pps+4, sizeof(sps_pps)-4);
		if (len > 0)
		{
		    if ((sps_pps[4] & 0x1f) == 7)
            {
        	    memcpy(p_sps, sps_pps, len+4);
        	    *sps_len = len+4;
        	}
        	else if ((sps_pps[4] & 0x1f) == 8)
        	{
        	    memcpy(p_pps, sps_pps, len+4);
        		*pps_len = len+4;
        	}
		}
	}

	return TRUE;
}

BOOL CRtsp::get_h264_sdp_desc(char * p_sdp, int max_len)
{
	return rua_get_sdp_h264_desc(&m_rua, NULL, p_sdp, max_len);
}

BOOL CRtsp::get_h265_sdp_desc(char * p_sdp, int max_len)
{
	return rua_get_sdp_h265_desc(&m_rua, NULL, p_sdp, max_len);
}

BOOL CRtsp::get_mp4_sdp_desc(char * p_sdp, int max_len)
{
	return rua_get_sdp_mp4_desc(&m_rua, NULL, p_sdp, max_len);
}

BOOL CRtsp::get_aac_sdp_desc(char * p_sdp, int max_len)
{
	return rua_get_sdp_aac_desc(&m_rua, NULL, p_sdp, max_len);
}

void CRtsp::get_h265_params()
{
	rtsp_send_h265_params(&m_rua);
}

BOOL CRtsp::get_h265_params(uint8 * p_sps, int * sps_len, uint8 * p_pps, int * pps_len, uint8 * p_vps, int * vps_len)
{
    int pt;
    BOOL don;
	char vps[1000] = {'\0'}, sps[1000] = {'\0'}, pps[1000] = {'\0'};

	if (!rua_get_sdp_h265_params(&m_rua, &pt, &don, vps, sizeof(vps), sps, sizeof(sps), pps, sizeof(pps)))
	{
		return FALSE;
	}

	uint8 buff[1000];
	buff[0] = 0x0;
	buff[1] = 0x0;
	buff[2] = 0x0;
	buff[3] = 0x1;

	if (vps[0] != '\0')
	{
		int len = base64_decode(vps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return FALSE;
		}

        memcpy(p_vps, buff, len+4);
	    *vps_len = len+4;
	}

	if (sps[0] != '\0')
	{
		int len = base64_decode(sps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return FALSE;
		}

		memcpy(p_sps, buff, len+4);
	    *sps_len = len+4;
	}

	if (pps[0] != '\0')
	{
		int len = base64_decode(pps, buff+4, sizeof(buff)-4);
		if (len <= 0)
		{
			return FALSE;
		}

		memcpy(p_pps, buff, len+4);
	    *pps_len = len+4;
	}

	return TRUE;
}

BOOL CRtsp::rtsp_get_video_media_info()
{
	if (m_rua.remote_video_cap_count == 0)
	{
		return FALSE;
	}

	if (m_rua.remote_video_cap[0] == 26)
	{
		m_VideoCodec = VIDEO_CODEC_JPEG;
	}

	int i;
	int rtpmap_len = (int)strlen("a=rtpmap:");

	for (i=0; i<MAX_AVN; i++)
	{
		char * ptr = m_rua.remote_video_cap_desc[i];
		if (memcmp(ptr, "a=rtpmap:", rtpmap_len) == 0)
		{
			char pt_buf[16];
			char code_buf[64];
			int next_offset = 0;
			ptr += rtpmap_len;

			if (GetLineWord(ptr, 0, (int)strlen(ptr), pt_buf, sizeof(pt_buf), &next_offset, WORD_TYPE_NUM) == FALSE)
				return FALSE;

			GetLineWord(ptr, next_offset, (int)strlen(ptr)-next_offset, code_buf, sizeof(code_buf),  &next_offset, WORD_TYPE_STRING);

			if (stricmp(code_buf, "H264/90000") == 0)
			{
				m_VideoCodec = VIDEO_CODEC_H264;
			}
			else if (stricmp(code_buf, "JPEG/90000") == 0)
			{
				m_VideoCodec = VIDEO_CODEC_JPEG;
			}
			else if (stricmp(code_buf, "MP4V-ES/90000") == 0)
			{
				m_VideoCodec = VIDEO_CODEC_MP4;
			}
			else if (stricmp(code_buf, "H265/90000") == 0)
			{
				m_VideoCodec = VIDEO_CODEC_H265;
			}

			break;
		}
	}

	return TRUE;
}

BOOL CRtsp::rtsp_get_audio_media_info()
{
	if (m_rua.remote_audio_cap_count == 0)
	{
		return FALSE;
	}

	if (m_rua.remote_audio_cap[0] == 0)
	{
		m_AudioCodec = AUDIO_CODEC_G711U;
	}
	else if (m_rua.remote_audio_cap[0] == 8)
	{
		m_AudioCodec = AUDIO_CODEC_G711A;
	}
	else if (m_rua.remote_audio_cap[0] == 2)
	{
		m_AudioCodec = AUDIO_CODEC_G726;
	}
	else if (m_rua.remote_audio_cap[0] == 9)
	{
		m_AudioCodec = AUDIO_CODEC_G722;
	}

	int i;
	int rtpmap_len = (int)strlen("a=rtpmap:");

	for (i=0; i<MAX_AVN; i++)
	{
		char * ptr = m_rua.remote_audio_cap_desc[i];
		if (memcmp(ptr, "a=rtpmap:", rtpmap_len) == 0)
		{
			char pt_buf[16];
			char code_buf[64];
			int next_offset = 0;

			ptr += rtpmap_len;

			if (GetLineWord(ptr, 0, (int)strlen(ptr), pt_buf, sizeof(pt_buf), &next_offset, WORD_TYPE_NUM) == FALSE)
			{
				return FALSE;
			}

			GetLineWord(ptr, next_offset, (int)strlen(ptr)-next_offset, code_buf, sizeof(code_buf),  &next_offset, WORD_TYPE_STRING);

			uppercase(code_buf);

			if (strstr(code_buf, "G726-32"))
			{
				m_AudioCodec = AUDIO_CODEC_G726;
			}
			else if (strstr(code_buf, "G722"))
			{
				m_AudioCodec = AUDIO_CODEC_G722;
			}
			else if (strstr(code_buf, "PCMU"))
			{
				m_AudioCodec = AUDIO_CODEC_G711U;
			}
			else if (strstr(code_buf, "PCMA"))
			{
				m_AudioCodec = AUDIO_CODEC_G711A;
			}
			else if (strstr(code_buf, "MPEG4-GENERIC"))
			{
				m_AudioCodec = AUDIO_CODEC_AAC;
			}
			else if (strstr(code_buf, "OPUS"))
			{
				m_AudioCodec = AUDIO_CODEC_OPUS;
			}

			char * p = strchr(code_buf, '/');
			if (p)
			{
				p++;

				char * p1 = strchr(p, '/');
				if (p1)
				{
					*p1 = '\0';
					m_nSamplerate = atoi(p);

					p1++;
					if (p1 && *p1 != '\0')
					{
						m_nChannels = atoi(p1);
					}
					else
					{
						m_nChannels = 1;
					}
				}
				else
				{
					m_nSamplerate = atoi(p);
					m_nChannels = 1;
				}
			}

			break;
		}
	}

    if (m_AudioCodec == AUDIO_CODEC_G722)
    {
        m_nSamplerate = 16000;
        m_nChannels = 1;
    }

	return TRUE;
}

void CRtsp::set_notify_cb(notify_cb notify, void * userdata)
{
	sys_os_mutex_enter(m_pMutex);
	m_pNotify = notify;
	m_pUserdata = userdata;
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::set_video_cb(video_cb cb)
{
	sys_os_mutex_enter(m_pMutex);
	m_pVideoCB = cb;
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::set_audio_cb(audio_cb cb)
{
	sys_os_mutex_enter(m_pMutex);
	m_pAudioCB = cb;
	sys_os_mutex_leave(m_pMutex);
}

void CRtsp::set_rtp_multicast(int flag)
{
	m_rua.mast_flag = flag;
}

void CRtsp::set_redirect_cb(redirect_cb cb)
{
	sys_os_mutex_enter(m_pMutex);
	m_pRedirectCB = cb;
	sys_os_mutex_leave(m_pMutex);
}
