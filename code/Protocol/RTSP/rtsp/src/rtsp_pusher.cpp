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
#include "rtsp_pusher.h"
#include "rtsp_cfg.h"
#include "media_format.h"
#include "rtp.h"
#include "media_util.h"
#include "base64.h"
#include "h264_util.h"
#include "h265_util.h"
#include "bit_vector.h"
#include "mjpeg.h"

#ifdef PUSHER

/*************************************************************/

RTSP_PUSHER * rtsp_add_pusher(RTSP_PUSHER ** p_pusher)
{
	RTSP_PUSHER * p_tmp;
	RTSP_PUSHER * p_new_pusher = (RTSP_PUSHER *) malloc(sizeof(RTSP_PUSHER));
	if (NULL == p_new_pusher)
	{
		return NULL;
	}

	memset(p_new_pusher, 0, sizeof(RTSP_PUSHER));

	p_tmp = *p_pusher;
	if (NULL == p_tmp)
	{
		*p_pusher = p_new_pusher;
	}
	else
	{
		while (p_tmp && p_tmp->next)
		{
			p_tmp = p_tmp->next;
		}
		
		p_tmp->next = p_new_pusher;
	}

	return p_new_pusher;
}

void rtsp_free_pushers(RTSP_PUSHER ** p_pusher)
{
	RTSP_PUSHER * p_next;
	RTSP_PUSHER * p_tmp = *p_pusher;

	while (p_tmp)
	{
		p_next = p_tmp->next;
		
		if (p_tmp->pusher)
		{
		   	delete p_tmp->pusher;
		    p_tmp->pusher = NULL;
		}
		
		free(p_tmp);
		p_tmp = p_next;
	}

	*p_pusher = NULL;
}

BOOL rtsp_init_pusher(RTSP_PUSHER * p_pusher)
{
    BOOL ret = FALSE;

    p_pusher->pusher = new CRtspPusher(&p_pusher->cfg);
    if (NULL == p_pusher->pusher)
    {
        log_print(LOG_ERR, "%s, new rtsp pusher failed\r\n", __FUNCTION__);
    }

    return ret;
}

void rtsp_init_pushers()
{
    RTSP_PUSHER * p_pusher = g_rtsp_cfg.pusher;
    while (p_pusher)
    {
        rtsp_init_pusher(p_pusher);
        
        p_pusher = p_pusher->next;
    }
}

int rtsp_get_pusher_nums()
{
    int nums = 0;
    
    RTSP_PUSHER * p_pusher = g_rtsp_cfg.pusher;
    while (p_pusher)
    {
        nums++;
        
        p_pusher = p_pusher->next;
    }

    return nums;
}

RTSP_PUSHER * rtsp_pusher_match(const char * suffix)
{
    RTSP_PUSHER * p_pusher = g_rtsp_cfg.pusher;
    while (p_pusher)
    {
        if (strcmp(suffix, p_pusher->cfg.suffix) == 0)
        {
            return p_pusher;
        }
        
        p_pusher = p_pusher->next;
    }
    
    return NULL;
}

void * rtsp_pusher_tcp_rx(void * argv)
{
	CRtspPusher * p_pusher = (CRtspPusher *)argv;

	p_pusher->tcpRecvThread();

	return NULL;
}

void * rtsp_pusher_udp_rx(void * argv)
{
	CRtspPusher * p_pusher = (CRtspPusher *)argv;

	p_pusher->udpRecvThread();

	return NULL;
}

int video_data_rx(uint8 * p_data, int len, uint32 ts, uint32 seq, void * p_userdata)
{
	CRtspPusher * p_pusher = (CRtspPusher *)p_userdata;
    PUSHER_CFG * p_config = p_pusher->getConfig();
    
    if (p_config->v_info.width == 0 || p_config->v_info.height == 0)
    {
        p_pusher->parseVideoSize(p_data, len);
    }
    
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int recodec = p_pusher->needVideoRecodec(p_data, len);
    if (1 == recodec) // need recodec
    {
        p_pusher->videoDataDecode(p_data, len);
    }
    else if (-1 == recodec) // not need recodec
    {
        p_pusher->videoDataRx(p_data, len);
    }
    else if (0 == recodec)  // still not sure
    {
    }
#else
    p_pusher->videoDataRx(p_data, len);
#endif	

	return 0;
}

int audio_data_rx(uint8 * p_data, int len, uint32 ts, uint32 seq, void * p_userdata)
{
	CRtspPusher * p_pusher = (CRtspPusher *)p_userdata;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int recodec = p_pusher->needAudioRecodec();
    if (1 == recodec) // need recodec
    {
        p_pusher->audioDataDecode(p_data, len);
    }
    else if (-1 == recodec) // not need recodec
    {
        p_pusher->audioDataRx(p_data, len);
    }
    else if (0 == recodec)  // still not sure
    {
    }
#else
    p_pusher->audioDataRx(p_data, len);
#endif

	return 0;
}

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)

void rtsp_pusher_vdecoder_cb(AVFrame * frame, void *pUserdata)
{
    CRtspPusher * pPusher = (CRtspPusher *)pUserdata;

    pPusher->videoDataEncode(frame);
}

void rtsp_pusher_vencoder_cb(uint8 *data, int size, void *pUserdata)
{
	CRtspPusher * pPusher = (CRtspPusher *)pUserdata;

	pPusher->videoDataRx(data, size);
}

void rtsp_pusher_adecoder_cb(AVFrame * frame, void *pUserdata)
{
    CRtspPusher * pPusher = (CRtspPusher *)pUserdata;

    pPusher->audioDataEncode(frame);
}

void rtsp_pusher_aencoder_cb(uint8 *data, int size, int nbsamples, void *pUserdata)
{
	CRtspPusher * pPusher = (CRtspPusher *)pUserdata;

	pPusher->audioDataRx(data, size);
}

#endif


/*************************************************************/
CRtspPusher::CRtspPusher(PUSHER_CFG * pConfig)
: m_pConfig(pConfig)
, m_bInited(FALSE)
, m_rua(NULL)
, m_bRecving(FALSE)
, m_tidRecv(0)
{    
	m_pCallbackMutex = sys_os_create_mutex();
    m_pCallbackList = h_list_create(FALSE);

	m_pRuaMutex = sys_os_create_mutex();
	
	memset(&m_vua, 0, sizeof(m_vua));
	memset(&m_aua, 0, sizeof(m_aua));

	h265_rxi_init(&m_vua.h265rxi, video_data_rx, this);
	aac_rxi_init(&m_aua.aacrxi, audio_data_rx, this);

	m_aua.aacrxi.size_length = 13;
	m_aua.aacrxi.index_length = 3;
	m_aua.aacrxi.index_delta_length = 3;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    m_nVideoRecodec = 0;
    m_nAudioRecodec = 0;
    m_pVideoDecoder = NULL;
    m_pAudioDecoder = NULL;
    m_pVideoEncoder = NULL;
	m_pAudioEncoder = NULL;
#endif

    initPusher();
}

CRtspPusher::~CRtspPusher(void)
{
	m_bRecving = FALSE;

	while (m_tidRecv)
	{
		usleep(10*1000);
	}
	
	if (m_vua.sfd > 0)
	{
		closesocket(m_vua.sfd);
		m_vua.sfd = 0;
	}

	if (m_aua.sfd > 0)
	{
		closesocket(m_aua.sfd);
		m_aua.sfd = 0;
	}

	closeMedia(&m_vua);
	closeMedia(&m_aua);

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if (m_pVideoDecoder)
    {
        delete m_pVideoDecoder;
        m_pVideoDecoder = NULL;
    }

    if (m_pAudioDecoder)
    {
        delete m_pAudioDecoder;
        m_pAudioDecoder = NULL;
    }

    if (m_pVideoEncoder)
    {
        delete m_pVideoEncoder;
        m_pVideoEncoder = NULL;
    }

    if (m_pAudioEncoder)
    {
        delete m_pAudioEncoder;
        m_pAudioEncoder = NULL;
    }
#endif
	
    h_list_free_container(m_pCallbackList);
    
    sys_os_destroy_sig_mutex(m_pCallbackMutex);
    sys_os_destroy_sig_mutex(m_pRuaMutex);
}

void CRtspPusher::initPusher()
{
	if (m_pConfig->transfer.ip == 0)
	{
		m_pConfig->transfer.ip = get_default_if_ip();
	}
	
	if (m_pConfig->has_video)
	{
		if (m_pConfig->transfer.mode != TRANSFER_MODE_RTSP)
		{
			if (m_pConfig->transfer.vport <= 0 || m_pConfig->transfer.vport >= 65535)
			{
				return;
			}
			
			m_vua.sfd = initSocket(m_pConfig->transfer.ip, m_pConfig->transfer.vport, m_pConfig->transfer.mode);
			if (m_vua.sfd <= 0)
			{
				return;
			}
		}
	}
	
	if (m_pConfig->has_audio)
	{
		if (m_pConfig->transfer.mode != TRANSFER_MODE_RTSP)
		{
			if (m_pConfig->transfer.aport <= 0 || m_pConfig->transfer.aport >= 65535)
			{
				return;
			}
			
			m_aua.sfd = initSocket(m_pConfig->transfer.ip, m_pConfig->transfer.aport, m_pConfig->transfer.mode);
			if (m_aua.sfd <= 0)
			{
				return;
			}
		}
	}

	m_bInited = startPusher();
}

int CRtspPusher::initSocket(uint32 ip, int port, int mode)
{
	struct sockaddr_in saddr;
	SOCKET fd = -1;

	if (mode == TRANSFER_MODE_TCP)
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
	}
	else if (mode == TRANSFER_MODE_UDP)
	{
		fd = socket(AF_INET, SOCK_DGRAM, 0);
	}
	
	if (fd <= 0)
	{
		return -1;
	}

	int len = 1024 * 1024;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&len, sizeof(int)))
	{
		log_print(H_LOG_WARN, "%s, setsockopt SO_SNDBUF error!!!\r\n", __FUNCTION__);
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&len, sizeof(int)))
	{
		log_print(H_LOG_WARN, "%s, setsockopt SO_SNDBUF error!!!\r\n", __FUNCTION__);
	}
		
	memset(&saddr, 0, sizeof(struct sockaddr_in));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
	saddr.sin_addr.s_addr = ip;

    if (bind(fd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in)) < 0)
	{
		log_print(LOG_ERR, "%s, bind errno=%d!!!\r\n", __FUNCTION__, errno);
		closesocket(fd);
		return -1;
	}

	if (mode == TRANSFER_MODE_TCP)
	{
	    if (listen(fd, 5) < 0)
		{
			log_print(LOG_ERR, "%s, listen errno=%d!!!\r\n", __FUNCTION__, errno);
			closesocket(fd);
			return -1;
		}
	}

	return fd;
}

void CRtspPusher::reinitMedia()
{
	closeMedia(&m_vua);
	closeMedia(&m_aua);

	h265_rxi_init(&m_vua.h265rxi, video_data_rx, this);
	aac_rxi_init(&m_aua.aacrxi, audio_data_rx, this);

	m_aua.aacrxi.size_length = 13;
	m_aua.aacrxi.index_length = 3;
	m_aua.aacrxi.index_delta_length = 3;
}

BOOL CRtspPusher::startPusher()
{
	m_bRecving = TRUE;

	if (m_pConfig->transfer.mode == TRANSFER_MODE_TCP)
	{
		m_tidRecv = sys_os_create_thread((void *)rtsp_pusher_tcp_rx, this);
	}
	else if (m_pConfig->transfer.mode == TRANSFER_MODE_UDP)
	{
		m_tidRecv = sys_os_create_thread((void *)rtsp_pusher_udp_rx, this);
	}
	
	return TRUE;
}

void CRtspPusher::tcpRecvThread()
{   
    fd_set fdr;
    int max_fd = 0;

	log_print(LOG_DBG, "%s, start\r\n", __FUNCTION__);

	while (m_bRecving)
	{
		FD_ZERO(&fdr);

		if (m_vua.sfd > 0)
		{
			FD_SET(m_vua.sfd, &fdr);			
			max_fd = (m_vua.sfd > max_fd ? m_vua.sfd : max_fd);		
		}		

		if (m_aua.sfd > 0)
		{
			FD_SET(m_aua.sfd, &fdr);
			max_fd = (m_aua.sfd > max_fd ? m_aua.sfd : max_fd);			
		}		

		if (m_vua.mfd > 0)
		{
			FD_SET(m_vua.mfd, &fdr);
			max_fd = (m_vua.mfd > max_fd ? m_vua.mfd : max_fd);		
		}		

		if (m_aua.mfd > 0)
		{
			FD_SET(m_aua.mfd, &fdr);			
			max_fd = (m_aua.mfd > max_fd ? m_aua.mfd : max_fd);			
		}		

		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		
		int sret = select(max_fd+1, &fdr, NULL, NULL, &tv);
		if (sret == 0)
		{
			continue;
		}
		else if (sret < 0)
		{
			log_print(LOG_ERR, "%s, select err[%s], max fd[%d], sret[%d]!!!\r\n", 
				__FUNCTION__, sys_os_get_socket_error(), max_fd, sret);

			usleep(10*1000);				
			continue;
		}

		if (FD_ISSET(m_vua.sfd, &fdr))
		{
			tcpListenRx(&m_vua);
		}

		if (FD_ISSET(m_aua.sfd, &fdr))
		{
			tcpListenRx(&m_aua);
		}
		
		if (FD_ISSET(m_vua.mfd, &fdr))
		{
			if (!tcpDataRx(&m_vua, MEDIA_TYPE_VIDEO))
			{
				closeMedia(&m_vua);
				
				h265_rxi_init(&m_vua.h265rxi, video_data_rx, this);
			}
		}

		if (FD_ISSET(m_aua.mfd, &fdr))
		{
			if (!tcpDataRx(&m_aua, MEDIA_TYPE_AUDIO))
			{
				closeMedia(&m_aua);
				
				aac_rxi_init(&m_aua.aacrxi, audio_data_rx, this);

				m_aua.aacrxi.size_length = 13;
            	m_aua.aacrxi.index_length = 3;
            	m_aua.aacrxi.index_delta_length = 3;
			}
		}
	}

    m_tidRecv = 0;
    
	log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);
}

void CRtspPusher::tcpListenRx(PUA * p_ua)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	socklen_t size = sizeof(struct sockaddr_in);

	int cfd = accept(p_ua->sfd, (struct sockaddr *)&addr, &size);
	if (cfd < 0)
	{
		log_print(LOG_ERR, "%s, accept ret error[%d]!!!\r\n", __FUNCTION__, errno);
		return;
	}
	
	if (p_ua->mfd == 0)
	{
		p_ua->mfd = cfd;
	}
	else
	{
		closesocket(cfd);
	}
}

BOOL CRtspPusher::tcpDataRx(PUA * p_ua, int type)
{
	RILF * pRtPkt = NULL;
	int len = tcpPacketRx(p_ua, &pRtPkt);
	if (len < 0)
	{
		return FALSE;
	}
	
	if (len > 0 && pRtPkt)
	{
		uint8 * p_data = (uint8 *)pRtPkt + sizeof(RILF);
		int rlen = len - sizeof(RILF);

		if (type == MEDIA_TYPE_VIDEO)
		{
			videoRtpRx(p_ua, p_data, rlen);
		}
		else
		{
			audioRtpRx(p_ua, p_data, rlen);
		}
		
		free(pRtPkt);
	}

	return TRUE;
}

BOOL CRtspPusher::videoRtpRx(PUA * p_ua, uint8 * p_rtp, int rlen)
{
	int ret = -1;

	if (m_pConfig->v_info.codec == VIDEO_CODEC_H264)
	{
		ret = h264_rtp_rx(&p_ua->h264rxi, p_rtp, rlen);
	}
	else if (m_pConfig->v_info.codec == VIDEO_CODEC_H265)
	{
		ret = h265_rtp_rx(&p_ua->h265rxi, p_rtp, rlen);
	}
	else if (m_pConfig->v_info.codec == VIDEO_CODEC_MP4)
	{
		ret = mpeg4_rtp_rx(&p_ua->mpeg4rxi, p_rtp, rlen);
	}
	else if (m_pConfig->v_info.codec == VIDEO_CODEC_JPEG)
	{
		ret = mjpeg_rtp_rx(&p_ua->mjpegrxi, p_rtp, rlen);
	}
	else
	{
		log_print(LOG_ERR, "%s, unsupport video codec\r\n", __FUNCTION__, m_pConfig->v_info.codec);
		return FALSE;
	}

	return ret;
}

BOOL CRtspPusher::audioRtpRx(PUA * p_ua, uint8 * p_rtp, int rlen)
{
	int ret = -1;

	if (m_pConfig->a_info.codec == AUDIO_CODEC_G711A || 
		m_pConfig->a_info.codec == AUDIO_CODEC_G711U || 
		m_pConfig->a_info.codec == AUDIO_CODEC_G726 || 
		m_pConfig->a_info.codec == AUDIO_CODEC_G722 || 
		m_pConfig->a_info.codec == AUDIO_CODEC_OPUS)
	{
		ret = pcm_rtp_rx(&p_ua->pcmrxi, p_rtp, rlen);
	}
	else if (m_pConfig->a_info.codec == AUDIO_CODEC_AAC)
	{
		ret = aac_rtp_rx(&p_ua->aacrxi, p_rtp, rlen);
	}
	else
	{
		log_print(LOG_ERR, "%s, unsupport audio codec\r\n", __FUNCTION__, m_pConfig->a_info.codec);
		return FALSE;
	}

	return ret;
}

int CRtspPusher::tcpPacketRx(PUA * p_ua, RILF ** ppRtPkt)
{
	BOOL bRecvFinished = FALSE;

	uint32 offset = p_ua->recv_data_offset;
	if (offset < sizeof(RILF))
	{
		// The header information has not been received
		int len = recv(p_ua->mfd, p_ua->recv_buf + offset, sizeof(RILF) - offset, 0);
		if (len <= 0)
		{
			log_print(LOG_ERR, "%s, recv ret %d, offset=%d\r\n", __FUNCTION__, len, offset);
			return -1;
		}

		p_ua->recv_data_offset += len;

		if (p_ua->recv_data_offset == sizeof(RILF))
		{
			RILF * pNmHdr = (RILF *)p_ua->recv_buf;
			int nMsgTotalLen = ntohs(pNmHdr->rtp_len) + sizeof(RILF);
			if (nMsgTotalLen > MAX_RTP_LEN)
			{
				log_print(LOG_ERR, "%s, len[%d] > %d !!!\r\n", __FUNCTION__, nMsgTotalLen, MAX_RTP_LEN);
				return -1;
			}

			// Allocate dynamic receive buffer 
			p_ua->dyn_recv_buf = (char *)malloc(nMsgTotalLen);
			if (p_ua->dyn_recv_buf == NULL)
			{
				log_print(LOG_ERR, "%s, dyn_recv_buf len = %d memory failed!!!\r\n", __FUNCTION__, nMsgTotalLen);
				return -1;
			}

			// Copy the header information to the dynamic buffer, and the offset remains unchanged.
			memcpy(p_ua->dyn_recv_buf, p_ua->recv_buf, sizeof(RILF));
			
			if (pNmHdr->rtp_len == 0)
			{
				bRecvFinished = TRUE;
			}
		}
		else if (p_ua->recv_data_offset > sizeof(RILF))
		{
			log_print(LOG_ERR, "%s, recv_data_offset[%u],offset[%u],len[%u]!!!\r\n", 
				__FUNCTION__, p_ua->recv_data_offset, offset, len);
		}
	}
	else
	{
		RILF * pNmHdr = (RILF *)p_ua->dyn_recv_buf;
		if (pNmHdr == NULL)
		{
			log_print(LOG_ERR, "%s, recv_data_offset[%u],dyn_recv_buf is null!!!\r\n", __FUNCTION__, p_ua->recv_data_offset);
			return -1;
		}

		uint32 nMsgTotalLen = ntohs(pNmHdr->rtp_len) + sizeof(RILF);
		if(nMsgTotalLen > MAX_RTP_LEN)
		{
			log_print(LOG_ERR, "%s, len[%d] > %d !!!\r\n", __FUNCTION__, nMsgTotalLen, MAX_RTP_LEN);
			return -1;
		}

		if (nMsgTotalLen > p_ua->recv_data_offset)
		{
			int len = recv(p_ua->mfd, p_ua->dyn_recv_buf + p_ua->recv_data_offset, nMsgTotalLen - p_ua->recv_data_offset, 0);
			if (len <= 0)
			{
				log_print(LOG_ERR, "%s, recv ret %d, offset=%d\r\n", __FUNCTION__, len, p_ua->recv_data_offset);
				return -1;
			}

			p_ua->recv_data_offset += len;
		}

		if (p_ua->recv_data_offset >= nMsgTotalLen)
		{
			bRecvFinished = TRUE;
		}	
	}

	if (bRecvFinished == TRUE)
	{
		RILF * pNmHdr = (RILF *)p_ua->dyn_recv_buf;
		int nMsgTotalLen = ntohs(pNmHdr->rtp_len) + sizeof(RILF);

		// A message has been received

		p_ua->recv_data_offset = 0;
		p_ua->dyn_recv_buf = NULL;
		
		*ppRtPkt = pNmHdr;

		return nMsgTotalLen;
	}

	return 0;
}

void CRtspPusher::videoDataRx(uint8 * p_data, int len)
{
#ifdef RTSP_CRYPT
	if (g_rtsp_cfg.crypt)
	{
		rtsp_crypt_data_decrypt(p_data, len);
	}
#endif

	dataCallback(p_data, len, DATA_TYPE_VIDEO);
}

void CRtspPusher::audioDataRx(uint8 * p_data, int len)
{
#ifdef RTSP_CRYPT
	if (g_rtsp_cfg.crypt)
	{
		rtsp_crypt_data_decrypt(p_data, len);
	}
#endif

	dataCallback(p_data, len, DATA_TYPE_AUDIO);
}

void CRtspPusher::udpRecvThread()
{
	fd_set fdr;
    int max_fd = 0;

	log_print(LOG_DBG, "%s, start\r\n", __FUNCTION__);

	while (m_bRecving)
	{
		FD_ZERO(&fdr);

		if (m_vua.sfd > 0)
		{
			FD_SET(m_vua.sfd, &fdr);
			max_fd = (m_vua.sfd > max_fd ? m_vua.sfd : max_fd);	
		}		

		if (m_aua.sfd > 0)
		{
			FD_SET(m_aua.sfd, &fdr);
			max_fd = (m_aua.sfd > max_fd ? m_aua.sfd : max_fd);			
		}		

		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		
		int sret = select(max_fd+1, &fdr, NULL, NULL, &tv);
		if (sret == 0)
		{
			continue;
		}
		else if (sret < 0)
		{
			log_print(LOG_ERR, "%s, select err[%s], max fd[%d], sret[%d]!!!\r\n", 
				__FUNCTION__, sys_os_get_socket_error(), max_fd, sret);

			usleep(10*1000);				
			continue;
		}

		if (FD_ISSET(m_vua.sfd, &fdr))
		{
			if (!udpDataRx(&m_vua, MEDIA_TYPE_VIDEO))
			{
				closeMedia(&m_vua);

				h265_rxi_init(&m_vua.h265rxi, video_data_rx, this);	
			}
			else if (m_rua)
			{
			    sys_os_mutex_enter(m_pRuaMutex);
			    
			    RSUA * p_rua = (RSUA *) m_rua;
			    if (p_rua)
                {
			        p_rua->lats_rx_time = time(NULL);
                }
                
			    sys_os_mutex_leave(m_pRuaMutex);
			}
		}

		if (FD_ISSET(m_aua.sfd, &fdr))
		{
			if (!udpDataRx(&m_aua, MEDIA_TYPE_AUDIO))
			{
				closeMedia(&m_aua);

				aac_rxi_init(&m_aua.aacrxi, audio_data_rx, this);

				m_aua.aacrxi.size_length = 13;
            	m_aua.aacrxi.index_length = 3;
            	m_aua.aacrxi.index_delta_length = 3;				
			}
			else if (m_rua)
			{
			    sys_os_mutex_enter(m_pRuaMutex);
			    
			    RSUA * p_rua = (RSUA *) m_rua;
                if (p_rua)
                {
			        p_rua->lats_rx_time = time(NULL);
                }

			    sys_os_mutex_leave(m_pRuaMutex);
			}
		}
	}

    m_tidRecv = 0;
    
	log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);
}

BOOL CRtspPusher::udpDataRx(PUA * p_ua, int type)
{
	char * p_buf = NULL;
	int buf_len = 0;
	char buf_tmp[2048];

	p_buf = buf_tmp + 32;
	buf_len = sizeof(buf_tmp) - 32;

	struct sockaddr_in addr;
	int addr_len = sizeof(struct sockaddr_in);

	if (p_ua->sfd <= 0)
	{
		return FALSE;
	}
	
	int rlen = recvfrom(p_ua->sfd, p_buf, buf_len, 0, (struct sockaddr *)&addr,(socklen_t *)&addr_len);
	if (rlen <= 0)
	{
		log_print(LOG_ERR, "%s, ret %d,err=[%s]!!!\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());
		return FALSE;
	}

	if (type == MEDIA_TYPE_VIDEO)
	{
		videoRtpRx(p_ua, (uint8*)p_buf, rlen);
	}	
	else if(type == MEDIA_TYPE_AUDIO)
	{
		audioRtpRx(p_ua, (uint8*)p_buf, rlen);
	}
	
	return TRUE;
}

void CRtspPusher::closeMedia(PUA * p_ua)
{
	if (p_ua->mfd)
	{
		closesocket(p_ua->mfd);
		p_ua->mfd = 0;
	}

	if (p_ua->dyn_recv_buf)
	{
		free(p_ua->dyn_recv_buf);
		p_ua->dyn_recv_buf = NULL;
	}

	p_ua->recv_data_offset = 0;

	h265_rxi_deinit(&p_ua->h265rxi);
}

BOOL CRtspPusher::isCallbackExist(PusherCallback pCallback, void *pUserdata)
{
	BOOL exist = FALSE;
	PusherCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (PusherCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata)
		{
			exist = TRUE;
			break;
		}
		
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);
	
	sys_os_mutex_leave(m_pCallbackMutex);

	return exist;
}

void CRtspPusher::addCallback(PusherCallback pCallback, void *pUserdata)
{
	if (isCallbackExist(pCallback, pUserdata))
	{
		return;
	}
	
	PusherCB * p_cb = (PusherCB *) malloc(sizeof(PusherCB));

	p_cb->pCallback = pCallback;
	p_cb->pUserdata = pUserdata;
	p_cb->bFirst = TRUE;

	sys_os_mutex_enter(m_pCallbackMutex);
	h_list_add_at_back(m_pCallbackList, p_cb);	
	sys_os_mutex_leave(m_pCallbackMutex);
}

void CRtspPusher::delCallback(PusherCallback pCallback, void * pUserdata)
{
	PusherCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (PusherCB *) p_node->p_data;
		if (p_cb->pCallback == pCallback && p_cb->pUserdata == pUserdata)
		{		
			free(p_cb);
			
			h_list_remove(m_pCallbackList, p_node);
			break;
		}
		
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

void CRtspPusher::dataCallback(uint8 * data, int size, int type)
{
	PusherCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (PusherCB *) p_node->p_data;
		if (p_cb->pCallback != NULL)
		{
			if (p_cb->bFirst && type == DATA_TYPE_VIDEO)
			{
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)			
			    if (NULL == m_pVideoEncoder)
#endif			    
			    {
    				if (m_pConfig->v_info.codec == VIDEO_CODEC_H264 && 
    					m_vua.h264rxi.param_sets.sps_len && 
    					m_vua.h264rxi.param_sets.pps_len)
    				{
    					p_cb->pCallback(m_vua.h264rxi.param_sets.sps, m_vua.h264rxi.param_sets.sps_len, type, p_cb->pUserdata);
    					p_cb->pCallback(m_vua.h264rxi.param_sets.pps, m_vua.h264rxi.param_sets.pps_len, type, p_cb->pUserdata);
    				}
    				else if (m_pConfig->v_info.codec == VIDEO_CODEC_H265 && 
    					m_vua.h265rxi.param_sets.vps_len && 
    					m_vua.h265rxi.param_sets.sps_len && 
    					m_vua.h265rxi.param_sets.pps_len)
    				{
    					p_cb->pCallback(m_vua.h265rxi.param_sets.vps, m_vua.h265rxi.param_sets.vps_len, type, p_cb->pUserdata);
    					p_cb->pCallback(m_vua.h265rxi.param_sets.sps, m_vua.h265rxi.param_sets.sps_len, type, p_cb->pUserdata);
    					p_cb->pCallback(m_vua.h265rxi.param_sets.pps, m_vua.h265rxi.param_sets.pps_len, type, p_cb->pUserdata);
    				}
				}
				
				p_cb->bFirst = FALSE;
			}
			
			p_cb->pCallback(data, size, type, p_cb->pUserdata);
		}
		
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

char * CRtspPusher::getH264AuxSDPLine(int rtp_pt)
{
    if (m_rua)
	{
		char sdp[1024] = {'\0'};
		
		if (!rsua_get_sdp_h264_desc((RSUA *)m_rua, NULL, sdp, sizeof(sdp)))
		{
			log_print(LOG_ERR, "%s, rsua_get_sdp_h264_desc failed\r\n", __FUNCTION__);
			return NULL;
		}

		char const* fmtpFmt = "a=fmtp:%d %s";
    	
	  	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    	+ 3 /* max char len */
	    	+ strlen(sdp);
	    	
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);

	  	sprintf(fmtp, fmtpFmt, rtp_pt, sdp);

	  	return fmtp;
	}
	else
	{
		uint8 * sps = m_vua.h264rxi.param_sets.sps; uint32 spsSize = m_vua.h264rxi.param_sets.sps_len;
		uint8 * pps = m_vua.h264rxi.param_sets.pps; uint32 ppsSize = m_vua.h264rxi.param_sets.pps_len;
		
		if (spsSize == 0 || ppsSize == 0)
		{
			return NULL;
		}

		// Set up the "a=fmtp:" SDP line for this stream:
		uint8* spsWEB = new uint8[spsSize]; // "WEB" means "Without Emulation Bytes"
		uint32 spsWEBSize = remove_emulation_bytes(spsWEB, spsSize, sps, spsSize);
		if (spsWEBSize < 4) 
		{
			// Bad SPS size => assume our source isn't ready
			delete[] spsWEB;
			return NULL;
		}
		
		uint8 profileLevelId = (spsWEB[1]<<16) | (spsWEB[2]<<8) | spsWEB[3];
		delete[] spsWEB;

		char* sps_base64 = new char[spsSize*2+1];
		char* pps_base64 = new char[ppsSize*2+1];

		base64_encode(sps, spsSize, sps_base64, spsSize*2+1);
		base64_encode(pps, ppsSize, pps_base64, ppsSize*2+1);
		
		char const* fmtpFmt =
			"a=fmtp:%d packetization-mode=1"
			";profile-level-id=%06X"
			";sprop-parameter-sets=%s,%s";
			
		uint32 fmtpFmtSize = strlen(fmtpFmt)
			+ 3 /* max char len */
			+ 6 /* 3 bytes in hex */
			+ strlen(sps_base64) + strlen(pps_base64);
			
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);
		
		sprintf(fmtp, fmtpFmt, rtp_pt, profileLevelId, sps_base64, pps_base64);

		delete[] sps_base64;
		delete[] pps_base64;

		return fmtp;

	}
}

char * CRtspPusher::getH265AuxSDPLine(int rtp_pt)
{
	if (m_rua)
	{
		char sdp[1024] = {'\0'};
		
		if (!rsua_get_sdp_h265_desc((RSUA *)m_rua, NULL, sdp, sizeof(sdp)))
		{
			log_print(LOG_ERR, "%s, rsua_get_sdp_h265_desc failed\r\n", __FUNCTION__);
			return NULL;
		}

		char const* fmtpFmt = "a=fmtp:%d %s";
    	
	  	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    	+ 3 /* max char len */
	    	+ strlen(sdp);
	    	
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);

	  	sprintf(fmtp, fmtpFmt, rtp_pt, sdp);

	  	return fmtp;
	}
	else
	{
		uint8* vps = m_vua.h265rxi.param_sets.vps; uint32 vpsSize = m_vua.h265rxi.param_sets.vps_len;
		uint8* sps = m_vua.h265rxi.param_sets.sps; uint32 spsSize = m_vua.h265rxi.param_sets.sps_len;
		uint8* pps = m_vua.h265rxi.param_sets.pps; uint32 ppsSize = m_vua.h265rxi.param_sets.pps_len;

		if (NULL == vps || vpsSize == 0 ||
			NULL == sps || spsSize == 0 ||
			NULL == pps || ppsSize == 0)
		{
			return NULL;
		}

		// Set up the "a=fmtp:" SDP line for this stream.
		uint8* vpsWEB = new uint8[vpsSize]; // "WEB" means "Without Emulation Bytes"
		uint32 vpsWEBSize = remove_emulation_bytes(vpsWEB, vpsSize, vps, vpsSize);
		if (vpsWEBSize < 6/*'profile_tier_level' offset*/ + 12/*num 'profile_tier_level' bytes*/)
		{
			// Bad VPS size => assume our source isn't ready
			delete[] vpsWEB;
			return NULL;
		}
		
		uint8 const* profileTierLevelHeaderBytes = &vpsWEB[6];
		uint32 profileSpace  = profileTierLevelHeaderBytes[0]>>6; // general_profile_space
		uint32 profileId = profileTierLevelHeaderBytes[0]&0x1F; 	// general_profile_idc
		uint32 tierFlag = (profileTierLevelHeaderBytes[0]>>5)&0x1;// general_tier_flag
		uint32 levelId = profileTierLevelHeaderBytes[11]; 		// general_level_idc
		uint8 const* interop_constraints = &profileTierLevelHeaderBytes[5];
		char interopConstraintsStr[100];
		sprintf(interopConstraintsStr, "%02X%02X%02X%02X%02X%02X", 
		interop_constraints[0], interop_constraints[1], interop_constraints[2],
		interop_constraints[3], interop_constraints[4], interop_constraints[5]);
		delete[] vpsWEB;

		char* sprop_vps = new char[vpsSize*2+1];
	  	char* sprop_sps = new char[spsSize*2+1];
	  	char* sprop_pps = new char[ppsSize*2+1];

		base64_encode(vps, vpsSize, sprop_vps, vpsSize*2+1);
	  	base64_encode(sps, spsSize, sprop_sps, spsSize*2+1);
		base64_encode(pps, ppsSize, sprop_pps, ppsSize*2+1);

		char const* fmtpFmt =
			"a=fmtp:%d profile-space=%u"
			";profile-id=%u"
			";tier-flag=%u"
			";level-id=%u"
			";interop-constraints=%s"
			";sprop-vps=%s"
			";sprop-sps=%s"
			";sprop-pps=%s";
			
		uint32 fmtpFmtSize = strlen(fmtpFmt)
			+ 3 /* max num chars: rtpPayloadType */ + 20 /* max num chars: profile_space */
			+ 20 /* max num chars: profile_id */
			+ 20 /* max num chars: tier_flag */
			+ 20 /* max num chars: level_id */
			+ strlen(interopConstraintsStr)
			+ strlen(sprop_vps)
			+ strlen(sprop_sps)
			+ strlen(sprop_pps);
			
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);
		
		sprintf(fmtp, fmtpFmt,
		  	rtp_pt, profileSpace,
			profileId,
			tierFlag,
			levelId,
			interopConstraintsStr,
			sprop_vps,
			sprop_sps,
			sprop_pps);

		delete[] sprop_vps;
		delete[] sprop_sps;
		delete[] sprop_pps;

		return fmtp;
	}
}

char * CRtspPusher::getMP4AuxSDPLine(int rtp_pt)
{
	if (m_rua)
	{
		char sdp[1024] = {'\0'};
		
		if (!rsua_get_sdp_mp4_desc((RSUA *)m_rua, NULL, sdp, sizeof(sdp)))
		{
			log_print(LOG_ERR, "%s, rsua_get_sdp_mp4_desc failed\r\n", __FUNCTION__);
			return NULL;
		}

		char const* fmtpFmt = "a=fmtp:%d %s";
    	
	  	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    	+ 3 /* max char len */
	    	+ strlen(sdp);
	    	
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);

	  	sprintf(fmtp, fmtpFmt, rtp_pt, sdp);

	  	return fmtp;
	}

	return NULL;
}

char * CRtspPusher::getAACAuxSDPLine(int rtp_pt)
{
	if (m_rua)
	{
		char sdp[1024] = {'\0'};
		
		if (!rsua_get_sdp_aac_desc((RSUA *)m_rua, NULL, sdp, sizeof(sdp)))
		{
			log_print(LOG_ERR, "%s, rsua_get_sdp_aac_desc failed\r\n", __FUNCTION__);
			return NULL;
		}

		char const* fmtpFmt = "a=fmtp:%d %s";
    	
	  	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    	+ 3 /* max char len */
	    	+ strlen(sdp);
	    	
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);

	  	sprintf(fmtp, fmtpFmt, rtp_pt, sdp);

	  	return fmtp;
	}
	else
	{
	    char const* fmtpFmt =
            "a=fmtp:%d "
        	"streamtype=5;profile-level-id=1;"
        	"mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3";
    	
	  	uint32 fmtpFmtSize = strlen(fmtpFmt)
	    	+ 3 /* max char len */;
	    	
		char* fmtp = new char[fmtpFmtSize+1];
		memset(fmtp, 0, fmtpFmtSize+1);

	  	sprintf(fmtp, fmtpFmt, rtp_pt);

	  	return fmtp;
	}
	
	return NULL;
}

char * CRtspPusher::getVideoAuxSDPLine(int rtp_pt)
{
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if (m_pVideoEncoder)
    {
        return m_pVideoEncoder->getAuxSDPLine(rtp_pt);
    }
	else
#endif	
	{
    	int codec = m_pConfig->v_info.codec;
    	char * sdp = NULL;

    	sys_os_mutex_enter(m_pRuaMutex);
    	
    	if (m_rua)
    	{
    		RSUA * p_rua = (RSUA *)m_rua;
    		
    		codec = p_rua->media_info.v_codec;
    	}
    	
    	if (codec == VIDEO_CODEC_H264)
    	{
    		sdp = getH264AuxSDPLine(rtp_pt);		
    	}
    	else if (codec == VIDEO_CODEC_H265)
    	{
    		sdp = getH265AuxSDPLine(rtp_pt);
    	}
    	else if (codec == VIDEO_CODEC_MP4)
    	{
    		sdp = getMP4AuxSDPLine(rtp_pt);
    	}

    	sys_os_mutex_leave(m_pRuaMutex);
    	
    	return sdp;  
	}
}

char * CRtspPusher::getAudioAuxSDPLine(int rtp_pt)
{
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    if (m_pAudioEncoder)
    {
        return m_pAudioEncoder->getAuxSDPLine(rtp_pt);
    }
    else
#endif
    {
    	int codec = m_pConfig->a_info.codec;
    	char * sdp = NULL;
    	
    	sys_os_mutex_enter(m_pRuaMutex);
    	
    	if (m_rua)
    	{
    		RSUA * p_rua = (RSUA *)m_rua;
    		
    		codec = p_rua->media_info.a_codec;
    	}
    	
    	if (codec == AUDIO_CODEC_AAC)
    	{
    		sdp = getAACAuxSDPLine(rtp_pt);		
    	}

    	sys_os_mutex_leave(m_pRuaMutex);
    	
    	return sdp;
	}
}


/*******************************************************************************/

void CRtspPusher::setRua(void * p_rua) 
{
	if (m_pConfig->transfer.mode != TRANSFER_MODE_RTSP)
	{
		return;
	}
	
	sys_os_mutex_enter(m_pRuaMutex);

	m_rua = p_rua;

	if (NULL == m_rua)
	{
		reinitMedia();
	}
	
	sys_os_mutex_leave(m_pRuaMutex);
}

void CRtspPusher::setAACConfig(int size_length, int index_length, int index_delta_length)
{
	m_aua.aacrxi.size_length = size_length;
	m_aua.aacrxi.index_length = index_length;
	m_aua.aacrxi.index_delta_length = index_delta_length;
}

void CRtspPusher::setMpeg4Config(uint8 * data, int len)
{
	m_vua.mpeg4rxi.hdr_len = len;
	memcpy(m_vua.mpeg4rxi.p_buf, data, len);
}

void CRtspPusher::setH264Params(char * p_sdp)
{
    char sps[1000] = {'\0'}, pps[1000] = {'\0'};

    char * p_substr = strstr(p_sdp, "sprop-parameter-sets=");
	if (p_substr != NULL)
	{
		p_substr += strlen("sprop-parameter-sets=");
		char * p_tmp = p_substr;

		while (*p_tmp != ' ' && *p_tmp != ';' && *p_tmp != '\0') 
		{
			p_tmp++;
		}
		
		int sps_base64_len = (int)(p_tmp - p_substr);
		if (sps_base64_len < sizeof(sps))
		{
			memcpy(sps, p_substr, sps_base64_len);
			sps[sps_base64_len] = '\0';
		}
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
	if (len > 0)
	{
		if ((sps_pps[4] & 0x1f) == 7)
        {
            if (len+4 < sizeof(m_vua.h264rxi.param_sets.sps))
            {
        	    memcpy(m_vua.h264rxi.param_sets.sps, sps_pps, len+4);
        	    m_vua.h264rxi.param_sets.sps_len = len+4;
    	    }
    	}
    	else if ((sps_pps[4] & 0x1f) == 8)
    	{
    	    if (len+4 < sizeof(m_vua.h264rxi.param_sets.pps))
    	    {
        	    memcpy(m_vua.h264rxi.param_sets.pps, sps_pps, len+4);
        		m_vua.h264rxi.param_sets.pps_len = len+4;
    		}
    	}
	}    
	
	if (pps[0] != '\0')
	{		
		len = base64_decode(pps, sps_pps+4, sizeof(sps_pps)-4);
		if (len > 0)
		{
		    if ((sps_pps[4] & 0x1f) == 7)
            {
                if (len+4 < sizeof(m_vua.h264rxi.param_sets.sps))
                {
            	    memcpy(m_vua.h264rxi.param_sets.sps, sps_pps, len+4);
            	    m_vua.h264rxi.param_sets.sps_len = len+4;
        	    }
        	}
        	else if ((sps_pps[4] & 0x1f) == 8)
        	{
        	    if (len+4 < sizeof(m_vua.h264rxi.param_sets.pps))
        	    {
            	    memcpy(m_vua.h264rxi.param_sets.pps, sps_pps, len+4);
            		m_vua.h264rxi.param_sets.pps_len = len+4;
        		}
        	}
		}
	}
}

void CRtspPusher::setH265Params(char * p_sdp)
{
	char vps[1000] = {'\0'}, sps[1000] = {'\0'}, pps[1000] = {'\0'};
	
	char * p_substr = strstr(p_sdp, "sprop-vps=");
	if (p_substr != NULL)
	{
		p_substr += strlen("sprop-vps=");
		char * p_tmp = p_substr;

		while (*p_tmp != ' ' && *p_tmp != ';' && *p_tmp != '\0')
		{
			p_tmp++;
		}
		
		int len = (int)(p_tmp - p_substr);
		if (len < sizeof(vps)-1)
		{
			memcpy(vps, p_substr, len);
			vps[len] = '\0';
		}
	}

	p_substr = strstr(p_sdp, "sprop-sps=");
	if (p_substr != NULL)
	{
		p_substr += strlen("sprop-sps=");
		char * p_tmp = p_substr;

		while (*p_tmp != ' ' && *p_tmp != ';' && *p_tmp != '\0')
		{
			p_tmp++;
		}
		
		int len = (int)(p_tmp - p_substr);
		if (len < sizeof(sps)-1)
		{
			memcpy(sps, p_substr, len);
			sps[len] = '\0';
		}
	}

	p_substr = strstr(p_sdp, "sprop-pps=");
	if (p_substr != NULL)
	{
		p_substr += strlen("sprop-pps=");
		char * p_tmp = p_substr;

		while (*p_tmp != ' ' && *p_tmp != ';' && *p_tmp != '\0')
		{
			p_tmp++;
		}
		
		int len = (int)(p_tmp - p_substr);
		if (len < sizeof(pps)-1)
		{
			memcpy(pps, p_substr, len);
			pps[len] = '\0';
		}
	}

	uint8 buff[1000];
	buff[0] = 0x0;
	buff[1] = 0x0;
	buff[2] = 0x0;
	buff[3] = 0x1;
	
	if (vps[0] != '\0')
	{
		int len = base64_decode(vps, buff+4, sizeof(buff)-4);
		if (len > 0)
		{
			memcpy(m_vua.h265rxi.param_sets.vps, buff, len+4);
	        m_vua.h265rxi.param_sets.vps_len = len+4;
		}
	}

	if (sps[0] != '\0')
	{
		int len = base64_decode(sps, buff+4, sizeof(buff)-4);
		if (len > 0)
		{
			memcpy(m_vua.h265rxi.param_sets.sps, buff, len+4);
	        m_vua.h265rxi.param_sets.sps_len = len+4;
		}
	}

	if (pps[0] != '\0')
	{
		int len = base64_decode(pps, buff+4, sizeof(buff)-4);
		if (len > 0)
		{
			memcpy(m_vua.h265rxi.param_sets.pps, buff, len+4);
	        m_vua.h265rxi.param_sets.pps_len = len+4;
		}
	}
}

BOOL CRtspPusher::startUdpRx(int vfd, int afd)
{
	if (m_pConfig->transfer.mode == TRANSFER_MODE_RTSP)
	{
		m_vua.sfd = vfd;
		m_aua.sfd = afd;
	
		m_bRecving = TRUE;
		m_tidRecv = sys_os_create_thread((void *)rtsp_pusher_udp_rx, this);

		return TRUE;
	}

	return FALSE;
}

void CRtspPusher::stopUdpRx()
{
	if (m_pConfig->transfer.mode != TRANSFER_MODE_RTSP)
	{
		return;
	}
	
	m_bRecving = FALSE;

	while (m_tidRecv)
	{
		usleep(10*1000);
	}
	
	reinitMedia();
}

BOOL CRtspPusher::videoRtpRx(uint8 * p_rtp, int rlen)
{
	return videoRtpRx(&m_vua, p_rtp, rlen);
}

BOOL CRtspPusher::audioRtpRx(uint8 * p_rtp, int rlen)
{
	return audioRtpRx(&m_aua, p_rtp, rlen);
}

void CRtspPusher::setVideoInfo(int has_video, int codec)
{
	m_pConfig->has_video = has_video;
	m_pConfig->v_info.codec = codec;
}

void CRtspPusher::setAudioInfo(int has_audio, int codec, int samplerate, int channels)
{
	m_pConfig->has_audio = has_audio;
	m_pConfig->a_info.codec = codec;
	m_pConfig->a_info.samplerate = samplerate;
	m_pConfig->a_info.channels = channels;
}

int CRtspPusher::parseVideoSize(uint8 * p_data, int len)
{
    uint8 nalu_t;

	if (p_data == NULL || len < 5)
	{
		return -1;
    }
    
	// Need to parse width X height

	if (VIDEO_CODEC_H264 == m_pConfig->v_info.codec)
	{        
		int s_len = 0, n_len = 0, parse_len = len;
		uint8 * p_cur = p_data;

		while (p_cur)
		{
			uint8 * p_next = avc_split_nalu(p_cur, parse_len, &s_len, &n_len);
			if (n_len < 5)
			{
				return 0;
            }
            
			nalu_t = (p_cur[s_len] & 0x1F);
			
			int b_start;
			nal_t nal;
			
			nal.i_payload = n_len-s_len-1;
			nal.p_payload = p_cur+s_len+1;
			nal.i_type = nalu_t;

			if (nalu_t == H264_NAL_SPS)	
			{
				h264_t parse;
				h264_parser_init(&parse);

				h264_parser_parse(&parse, &nal, &b_start);
				log_print(H_LOG_INFO, "%s, H264 width[%d],height[%d]\r\n", __FUNCTION__, parse.i_width, parse.i_height);
				m_pConfig->v_info.width = parse.i_width;
				m_pConfig->v_info.height = parse.i_height;
				return 0;
			}
			
			parse_len -= n_len;
			p_cur = p_next;
		}
	}
	else if (VIDEO_CODEC_H265 == m_pConfig->v_info.codec)
	{
		int s_len, n_len = 0, parse_len = len;
		uint8 * p_cur = p_data;

		while (p_cur)
		{
			uint8 * p_next = avc_split_nalu(p_cur, parse_len, &s_len, &n_len);
			if (n_len < 5)
			{
				return 0;
            }
            
			nalu_t = (p_cur[s_len] >> 1) & 0x3F;
			
			if (nalu_t == HEVC_NAL_SPS)	
			{
				h265_t parse;
				h265_parser_init(&parse);

				if (h265_parser_parse(&parse, p_cur+4, n_len-s_len) == 0)
				{
					log_print(H_LOG_INFO, "%s, H265 width[%d],height[%d]\r\n", __FUNCTION__, parse.pic_width_in_luma_samples, parse.pic_height_in_luma_samples);
					m_pConfig->v_info.width = parse.pic_width_in_luma_samples;
					m_pConfig->v_info.height = parse.pic_height_in_luma_samples;
					return 0;
				}
			}
			
			parse_len -= n_len;
			p_cur = p_next;
		}
	}
    else if (VIDEO_CODEC_JPEG == m_pConfig->v_info.codec)
    {
        int offset = 0;
        int size_chunk = 0;
        
        while (offset < len - 8 && p_data[offset] == 0xFF)
        {
            if (p_data[offset+1] == MARKER_SOF0)
            {
                int h = ((p_data[offset+5] << 8) | p_data[offset+6]);
                int w = ((p_data[offset+7] << 8) | p_data[offset+8]);
                log_print(H_LOG_INFO, "%s, MJPEG width[%d],height[%d]\r\n", __FUNCTION__, w, h);
                m_pConfig->v_info.width = w;
			    m_pConfig->v_info.height = h;
                break;
            }
            else if (p_data[offset+1] == MARKER_SOI)
            {
                offset += 2;
            }
            else
            {
                size_chunk = ((p_data[offset+2] << 8) | p_data[offset+3]);
                offset += 2 + size_chunk;
            }
        }
    }
    else if (VIDEO_CODEC_MP4 == m_pConfig->v_info.codec)
    {
        int pos = 0;
        int vol_f = 0;
        int vol_pos = 0;
        int vol_len = 0;

        while (pos < len - 4)
        {
            if (p_data[pos] == 0 && p_data[pos+1] == 0 && p_data[pos+2] == 1)
            {
                if (p_data[pos+3] >= 0x20 && p_data[pos+3] <= 0x2F)
                {
                    vol_f = 1;
                    vol_pos = pos+4;
                }
                else if (vol_f)
                {
                    vol_len = pos - vol_pos;
                    break;
                }
            }

            pos++;
        }

        if (!vol_f)
        {
            return 0;
        }
        else if (vol_len <= 0)
        {
            vol_len = len - vol_pos;
        }

        int vo_ver_id;
                    
        BitVector bv(&p_data[vol_pos], 0, vol_len*8);

        bv.skipBits(1);     /* random access */
        bv.skipBits(8);     /* vo_type */

        if (bv.get1Bit())   /* is_ol_id */
        {
            vo_ver_id = bv.getBits(4); /* vo_ver_id */
            bv.skipBits(3); /* vo_priority */
        }

        if (bv.getBits(4) == 15) // aspect_ratio_info
        {
            bv.skipBits(8);     // par_width
            bv.skipBits(8);     // par_height
        }

        if (bv.get1Bit()) /* vol control parameter */
        {
            int chroma_format = bv.getBits(2);
			UNUSED( chroma_format );

            bv.skipBits(1);     /* low_delay */

            if (bv.get1Bit()) 
            {    
                /* vbv parameters */
                bv.getBits(15);   /* first_half_bitrate */
                bv.skipBits(1);
                bv.getBits(15);   /* latter_half_bitrate */
                bv.skipBits(1);
                bv.getBits(15);   /* first_half_vbv_buffer_size */
                bv.skipBits(1);
                bv.getBits(3);    /* latter_half_vbv_buffer_size */
                bv.getBits(11);   /* first_half_vbv_occupancy */
                bv.skipBits(1);
                bv.getBits(15);   /* latter_half_vbv_occupancy */
                bv.skipBits(1);
            }                        
        }

        int shape = bv.getBits(2); /* vol shape */
        
        if (shape == 3 && vo_ver_id != 1) 
        {
            bv.skipBits(4);  /* video_object_layer_shape_extension */
        }

        bv.skipBits(1); 
        
        int framerate = bv.getBits(16);

        int time_increment_bits = (int) (log(framerate - 1.0) * 1.44269504088896340736 + 1); // log2(framerate - 1) + 1
        if (time_increment_bits < 1)
        {
            time_increment_bits = 1;
        }
        
        bv.skipBits(1);
        
        if (bv.get1Bit() != 0)     /* fixed_vop_rate  */
        {
            bv.skipBits(time_increment_bits);    
        }
        
        if (shape != 2)
        {
            if (shape == 0) 
            {
                bv.skipBits(1);
                int w = bv.getBits(13);
                bv.skipBits(1);
                int h = bv.getBits(13);

                log_print(H_LOG_INFO, "%s, MPEG4 width[%d],height[%d]\r\n", __FUNCTION__, w, h);
                m_pConfig->v_info.width = w;
    		    m_pConfig->v_info.height = h;
            }
        }        
    }
    
	return 0;
}

/***********************************************************************/

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)

/**
 * 1  - need
 * 0  - Still not sure
 * -1 - not need
 */
int CRtspPusher::needVideoRecodec(uint8 * p_data, int len)
{
    if (0 != m_nVideoRecodec)
    {
        return m_nVideoRecodec;    
    }
    
    if (!m_pConfig->has_output)
    {
        m_nVideoRecodec = -1;
        return -1;
    }
    
    if (m_pConfig->v_info.width == 0 || m_pConfig->v_info.height == 0)
    {
        if (VIDEO_CODEC_H264 == m_pConfig->v_info.codec)
        {
            if (m_vua.h264rxi.param_sets.sps_len > 0)
            {
                parseVideoSize(m_vua.h264rxi.param_sets.sps, m_vua.h264rxi.param_sets.sps_len);
            }
        }
        else if (VIDEO_CODEC_H265 == m_pConfig->v_info.codec)
        {
            if (m_vua.h265rxi.param_sets.sps_len > 0)
            {
                parseVideoSize(m_vua.h265rxi.param_sets.sps, m_vua.h265rxi.param_sets.sps_len);
            }
        }
    }

    if (m_pConfig->v_info.width == 0 || m_pConfig->v_info.height == 0)
    {
        return 0;
    }

    if (VIDEO_CODEC_NONE == m_pConfig->output.v_info.codec)
    {
        m_pConfig->output.v_info.codec = m_pConfig->v_info.codec;
    }

    if (0 == m_pConfig->output.v_info.width)
    {
        m_pConfig->output.v_info.width = m_pConfig->v_info.width;
    }

    if (0 == m_pConfig->output.v_info.height)
    {
        m_pConfig->output.v_info.height = m_pConfig->v_info.height;
    }

    if (0 == m_pConfig->output.v_info.framerate)
    {
        m_pConfig->output.v_info.framerate = 25;
    }
            
    if (m_pConfig->output.v_info.codec != m_pConfig->v_info.codec || 
        m_pConfig->output.v_info.width != m_pConfig->v_info.width ||
        m_pConfig->output.v_info.height != m_pConfig->v_info.height)
    {
        m_pVideoDecoder = new CVideoDecoder;
        if (NULL == m_pVideoDecoder)
		{
		    m_nVideoRecodec = -1;
		    log_print(LOG_ERR, "%s, new video decoder failed\r\n", __FUNCTION__);
			return -1;
		}

		if (!m_pVideoDecoder->init(m_pConfig->v_info.codec, -1))
		{
		    m_nVideoRecodec = -1;
		    log_print(LOG_ERR, "%s, video decoder init failed\r\n", __FUNCTION__);
		    return -1;
		}

		m_pVideoDecoder->setCallback(rtsp_pusher_vdecoder_cb, this);

		if (VIDEO_CODEC_H264 == m_pConfig->v_info.codec)
        {
            if (m_vua.h264rxi.param_sets.sps_len > 0)
            {
                m_pVideoDecoder->decode(m_vua.h264rxi.param_sets.sps, m_vua.h264rxi.param_sets.sps_len);
            }

            if (m_vua.h264rxi.param_sets.pps_len > 0)
            {
                m_pVideoDecoder->decode(m_vua.h264rxi.param_sets.pps, m_vua.h264rxi.param_sets.pps_len);
            }
        }
        else if (VIDEO_CODEC_H265 == m_pConfig->v_info.codec)
        {
            if (m_vua.h265rxi.param_sets.vps_len > 0)
            {
                m_pVideoDecoder->decode(m_vua.h265rxi.param_sets.vps, m_vua.h265rxi.param_sets.vps_len);
            }
            
            if (m_vua.h265rxi.param_sets.sps_len > 0)
            {
                m_pVideoDecoder->decode(m_vua.h265rxi.param_sets.sps, m_vua.h265rxi.param_sets.sps_len);
            }

            if (m_vua.h265rxi.param_sets.pps_len > 0)
            {
                m_pVideoDecoder->decode(m_vua.h265rxi.param_sets.pps, m_vua.h265rxi.param_sets.pps_len);
            }
        }

		m_pVideoEncoder = new CVideoEncoder;		
		if (NULL == m_pVideoEncoder)
		{
		    m_nVideoRecodec = -1;
		    log_print(LOG_ERR, "%s, new video encoder failed\r\n", __FUNCTION__);
			return -1;
		}

		VideoEncoderParam params;
		memset(&params, 0, sizeof(params));
		
		params.SrcWidth = m_pConfig->v_info.width;
		params.SrcHeight = m_pConfig->v_info.height;
		params.SrcPixFmt = AV_PIX_FMT_YUV420P;
		params.DstCodec = m_pConfig->output.v_info.codec;
		params.DstWidth = m_pConfig->output.v_info.width;
		params.DstHeight = m_pConfig->output.v_info.height;
		params.DstFramerate = m_pConfig->output.v_info.framerate;
		params.DstBitrate = m_pConfig->output.v_info.bitrate;
		
		if (FALSE == m_pVideoEncoder->init(&params))
		{
		    m_nVideoRecodec = -1;
		    log_print(LOG_ERR, "%s, video encoder init failed\r\n", __FUNCTION__);
			return -1;
		}

		m_pVideoEncoder->addCallback(rtsp_pusher_vencoder_cb, this);

		m_nVideoRecodec = 1;
    }
    else
    {
        m_nVideoRecodec = -1;
    }

    return m_nVideoRecodec;
}

/**
 * 1  - need
 * 0  - Still not sure
 * -1 - not need
 */
int CRtspPusher::needAudioRecodec()
{
    if (0 != m_nAudioRecodec)
    {
        return m_nAudioRecodec;    
    }
    
    if (!m_pConfig->has_output)
    {
        m_nAudioRecodec = -1;
        return -1;
    }

    if (AUDIO_CODEC_NONE == m_pConfig->output.a_info.codec)
    {
        m_pConfig->output.a_info.codec = m_pConfig->a_info.codec;
    }

    if (0 == m_pConfig->output.a_info.samplerate)
    {
        m_pConfig->output.a_info.samplerate = m_pConfig->a_info.samplerate;
    }

    if (0 == m_pConfig->output.a_info.channels)
    {
        m_pConfig->output.a_info.channels = m_pConfig->a_info.channels;
    }
            
    if (m_pConfig->output.a_info.codec != m_pConfig->a_info.codec || 
        m_pConfig->output.a_info.samplerate != m_pConfig->a_info.samplerate ||
        m_pConfig->output.a_info.channels != m_pConfig->a_info.channels)
    {
        m_pAudioDecoder = new CAudioDecoder;
        if (NULL == m_pAudioDecoder)
		{
		    m_nAudioRecodec = -1;
		    log_print(LOG_ERR, "%s, new audio decoder failed\r\n", __FUNCTION__);
			return -1;
		}

		if (!m_pAudioDecoder->init(m_pConfig->a_info.codec, m_pConfig->a_info.samplerate, m_pConfig->a_info.channels, NULL, 0))
		{
		    m_nAudioRecodec = -1;
		    log_print(LOG_ERR, "%s, audio decoder init failed\r\n", __FUNCTION__);
		    return -1;
		}

		m_pAudioDecoder->setCallback(rtsp_pusher_adecoder_cb, this);

		m_pAudioEncoder = new CAudioEncoder;		
		if (NULL == m_pAudioEncoder)
		{
		    m_nAudioRecodec = -1;
		    log_print(LOG_ERR, "%s, new audio encoder failed\r\n", __FUNCTION__);
			return -1;
		}

		AudioEncoderParam params;
		memset(&params, 0, sizeof(params));
		
		params.SrcSamplerate = m_pConfig->a_info.samplerate;
		params.SrcChannels = m_pConfig->a_info.channels;
		params.SrcSamplefmt = AV_SAMPLE_FMT_S16;
		params.DstCodec = m_pConfig->output.a_info.codec;
		params.DstSamplerate = m_pConfig->output.a_info.samplerate;
		params.DstChannels = m_pConfig->output.a_info.channels;
		params.DstSamplefmt = AV_SAMPLE_FMT_S16;
		params.DstBitrate = m_pConfig->output.a_info.bitrate;
		
		if (FALSE == m_pAudioEncoder->init(&params))
		{
		    m_nAudioRecodec = -1;
		    log_print(LOG_ERR, "%s, audio encoder init failed\r\n", __FUNCTION__);
			return -1;
		}

		m_pAudioEncoder->addCallback(rtsp_pusher_aencoder_cb, this);

		m_nAudioRecodec = 1;
    }
    else
    {
        m_nAudioRecodec = -1;
    }

    return m_nAudioRecodec;
}

void CRtspPusher::videoDataDecode(uint8 * p_data, int len)
{
    if (m_pVideoDecoder)
    {
        m_pVideoDecoder->decode(p_data, len);
    }
}

void CRtspPusher::videoDataEncode(AVFrame * frame)
{
    if (m_pVideoEncoder)
    {
        m_pVideoEncoder->encode(frame);
    }
}

void CRtspPusher::audioDataDecode(uint8 * p_data, int len)
{
    if (m_pAudioDecoder)
    {
        m_pAudioDecoder->decode(p_data, len);
    }
}

void CRtspPusher::audioDataEncode(AVFrame * frame)
{
    if (m_pAudioEncoder)
    {
        m_pAudioEncoder->encode(frame);
    }
}

#endif // end of defined(RTSP_FILE) || defined(RTSP_DEVICE)

#endif


