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
#include "rtsp_srv_backchannel.h"

#ifdef RTSP_BACKCHANNEL

#if __WINDOWS_OS__
#include "audio_play_win.h"
#endif

#if __LINUX_OS__
#include "audio_play_linux.h"
#endif

void rtsp_bc_audio_cb(AVFrame * frame, void * puser)
{
    RSUA * p_rua = (RSUA *) puser;

    if (p_rua->audio_player)
	{
		p_rua->audio_player->playAudio(frame->data[0], frame->nb_samples * frame->channels * av_get_bytes_per_sample((enum AVSampleFormat)frame->format));
	}
}

BOOL rtsp_bc_init_audio_play(RSUA * p_rua, int samplerate, int channels)
{
	if (p_rua->audio_player == NULL)
	{
#if __WINDOWS_OS__	
		p_rua->audio_player = new CWAudioPlay(samplerate, channels);
#endif
#if __LINUX_OS__
		p_rua->audio_player = new CLAudioPlay(samplerate, channels);
#endif
	}

	if (p_rua->audio_player == NULL)
	{
	    return FALSE;
	}

	p_rua->audio_player->setVolume(255);
	
	return p_rua->audio_player->startPlay();
}

BOOL rtsp_bc_init_audio(RSUA * p_rua)
{
    BOOL ret = FALSE;
    
    p_rua->audio_decoder = new CAudioDecoder();
	if (p_rua->audio_decoder)
	{	
		ret = p_rua->audio_decoder->init(p_rua->bc_codec, p_rua->bc_samplerate, p_rua->bc_channels, NULL, 0);
	}

	if (ret)
	{
		p_rua->audio_decoder->setCallback(rtsp_bc_audio_cb, p_rua);
		
		ret = rtsp_bc_init_audio_play(p_rua, p_rua->bc_samplerate, p_rua->bc_channels);
	}

	if (ret)
	{
	    p_rua->ad_inited = 1;
	}
	else
	{
	    rtsp_bc_uninit_audio(p_rua);
	}

	return ret;
}

void rtsp_bc_uninit_audio(RSUA * p_rua)
{
	if (NULL == p_rua)
	{
		return;
	}

	if (p_rua->audio_decoder)
	{
		delete p_rua->audio_decoder;
		p_rua->audio_decoder = NULL;
	}

	if (p_rua->audio_player)
	{
		delete p_rua->audio_player;
		p_rua->audio_player = NULL;
	}

	p_rua->ad_inited = 0;
}

BOOL rtsp_bc_audio_rx(RSUA * p_rua, uint8 * lpData, int rlen, uint32 seq, uint32 ts)
{
    if (p_rua->ad_inited && p_rua->audio_decoder)
	{
		p_rua->audio_decoder->decode(lpData, rlen);
	}

    return TRUE;
}

void rtsp_bc_tcp_data_rx(RSUA * p_rua, uint8 * lpData, int rlen)
{
	RILF * p_rilf = (RILF *)lpData;
	uint8 * p_rtp = (uint8 *)p_rilf + 4;
	uint32	rtp_len = rlen - 4;

	if (p_rilf->channel == p_rua->bc_interleaved)
	{
		if (AUDIO_CODEC_G726  == p_rua->bc_codec || 
		    AUDIO_CODEC_G711A == p_rua->bc_codec || 
		    AUDIO_CODEC_G711U == p_rua->bc_codec || 
		    AUDIO_CODEC_G722  == p_rua->bc_codec || 
		    AUDIO_CODEC_OPUS  == p_rua->bc_codec)
		{
			if (rtp_data_rx(&p_rua->rtprxi, p_rtp, rtp_len))
			{
				rtsp_bc_audio_rx(p_rua, p_rua->rtprxi.p_data, p_rua->rtprxi.len, p_rua->rtprxi.prev_seq, p_rua->rtprxi.prev_ts);
			}
		}
	}
}

void rtsp_bc_udp_data_rx(RSUA * p_rua, uint8 * lpData, int rlen)
{
	if (AUDIO_CODEC_G726  == p_rua->bc_codec || 
	    AUDIO_CODEC_G711A == p_rua->bc_codec || 
	    AUDIO_CODEC_G711U == p_rua->bc_codec || 
	    AUDIO_CODEC_G722  == p_rua->bc_codec || 
	    AUDIO_CODEC_OPUS  == p_rua->bc_codec)
	{
		if (rtp_data_rx(&p_rua->rtprxi, lpData, rlen))
		{
			rtsp_bc_audio_rx(p_rua, p_rua->rtprxi.p_data, p_rua->rtprxi.len, p_rua->rtprxi.prev_seq, p_rua->rtprxi.prev_ts);
		}
	}
}

void * rtsp_bc_udp_rx_thread(void * argv)
{
	fd_set fdr;
	RSUA * p_rua = (RSUA *)argv;
	int fd = p_rua->bc_udp_fd;
	
	while (p_rua->rtp_rx)
	{
		FD_ZERO(&fdr);
		FD_SET(fd, &fdr);

		struct timeval tv = {1, 0};

		int sret = select(fd+1, &fdr, NULL, NULL, &tv);
		if (sret == 0)
		{
			continue;
	    }
	    else if (sret < 0)
	    {
	    	usleep(10*1000);
	    	continue;
	    }

	    if (!FD_ISSET(fd, &fdr))
	    {
	    	continue;
	    }

	    int addr_len;
	    char buf[2048];
	    struct sockaddr_in addr;
	    
		memset(&addr, 0, sizeof(addr));
		addr_len = sizeof(struct sockaddr_in);

		int rlen = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, (socklen_t*)&addr_len);
    	if (rlen <= 12)
    	{
    		log_print(LOG_ERR, "%s, recvfrom return %d,err[%s]!!!\r\n", __FUNCTION__, rlen, sys_os_get_socket_error());
    	}
    	else
    	{
    	    rtsp_bc_udp_data_rx(p_rua, (uint8*)buf, rlen);
    	}
	}

	p_rua->tid_udp_rx = 0;

    log_print(LOG_DBG, "%s, exit!!!\r\n", __FUNCTION__);
	
	return NULL;
}

#endif // end of RTSP_BACKCHANNEL


