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
#include "audio_play_linux.h"


CLAudioPlay::CLAudioPlay(int samplerate, int channels) : CAudioPlay(samplerate, channels)
{
	memset(&m_alsa, 0, sizeof(m_alsa));
	
    m_alsa.period_size = 64;
    m_alsa.framesize = 16 / 8 * 2;
}

CLAudioPlay::~CLAudioPlay(void)
{
    stopPlay();    
}

BOOL CLAudioPlay::startPlay()
{
	if (alsa_get_device_name(0, m_alsa.devname, sizeof(m_alsa.devname)) == FALSE)
	{
		log_print(LOG_ERR, "get device name failed. %d\r\n", 0);
		return FALSE;
	}

	log_print(LOG_DBG, "%s, filename=%s\r\n", __FUNCTION__, m_alsa.devname);
	
	if (alsa_open_device(&m_alsa, SND_PCM_STREAM_PLAYBACK, (uint32 *)&m_nSamplerate, m_nChannels) == FALSE)
	{
		log_print(LOG_ERR, "open device (%s) failed\r\n", m_alsa.devname);
		return FALSE;
	}
	
	return TRUE;
}

void CLAudioPlay::stopPlay()
{
	if (m_alsa.handler)
	{
		snd_pcm_close(m_alsa.handler);
		m_alsa.handler = NULL;
	}
	
	m_bInited = FALSE;
}

int CLAudioPlay::setVolume(int volume)
{
    return 0;
}

int CLAudioPlay::getVolume()
{    
    return 0;
}
	
void CLAudioPlay::playAudio(uint8 * data, int size) 
{ 
	if (!m_bInited || NULL == m_alsa.handler)
	{
		return;
	}

	int res;
	int bufsize = size;
	
    bufsize /= m_alsa.framesize;

    while ((res = snd_pcm_writei(m_alsa.handler, data, bufsize)) < 0) 
    {
    	if (res == -EAGAIN) 
        {
            break;
        }
        
        if (alsa_xrun_recover(m_alsa.handler, res) < 0) 
        {
            break;
        }
    }
}




