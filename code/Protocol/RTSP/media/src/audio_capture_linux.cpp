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
#include "audio_capture_linux.h"
#include "lock.h"


/***************************************************************************************/
static void * audioCaptureThread(void * argv)
{
	CLAudioCapture *capture = (CLAudioCapture *)argv;

	capture->captureThread();

	return NULL;
}

/***************************************************************************************/

CLAudioCapture::CLAudioCapture() : CAudioCapture()
{
	memset(&m_alsa, 0, sizeof(m_alsa));
	
    m_alsa.period_size = 64;
    m_alsa.framesize = 16 / 8 * 2;
    
    m_hCapture = 0;
}

CLAudioCapture::~CLAudioCapture()
{
    stopCapture();
}

CAudioCapture * CLAudioCapture::getInstance(int devid)
{
	if (devid < 0 || devid >= MAX_AUDIO_DEV_NUMS)
	{
		return NULL;
	}
	
	if (NULL == m_pInstance[devid])
	{
		sys_os_mutex_enter(m_pInstMutex);

		if (NULL == m_pInstance[devid])
		{
			m_pInstance[devid] = new CLAudioCapture;
			if (m_pInstance[devid])
			{
				m_pInstance[devid]->m_nRefCnt++;
				m_pInstance[devid]->m_nDevIndex = devid;
			}
		}
		
		sys_os_mutex_leave(m_pInstMutex);
	}
	else
	{
		sys_os_mutex_enter(m_pInstMutex);
		m_pInstance[devid]->m_nRefCnt++;
		sys_os_mutex_leave(m_pInstMutex);
	}

	return m_pInstance[devid];
}

int CLAudioCapture::getDeviceNums()
{
	int count = 0;
    void **hints, **n;
    char *io = NULL;
    const char *filter = "Input";

    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
    {
        return 0;
    }
    
    n = hints;
    while (*n) 
    {
        io = snd_device_name_get_hint(*n, "IOID");

        if (!io || !strcmp(io, filter)) 
        {
            count++;
        }

        if (io)
        {
        	free(io);
        }
        
        n++;
    }

    if (hints)
    {
   		snd_device_name_free_hint(hints);
    }
    
    return count;
}

BOOL CLAudioCapture::initCapture(int codec, int samplerate, int channels, int bitrate)
{
	CLock lock(m_pMutex);
	
	if (m_bInited)
	{
		return TRUE;
	}

	if (alsa_get_device_name(m_nDevIndex, m_alsa.devname, sizeof(m_alsa.devname)) == FALSE)
	{
		log_print(LOG_ERR, "get device name failed. %d\r\n", m_nDevIndex);
		return FALSE;
	}

	log_print(LOG_DBG, "%s, m_nDevIndex=%d, filename=%s\r\n", __FUNCTION__, m_nDevIndex, m_alsa.devname);
	
	if (alsa_open_device(&m_alsa, SND_PCM_STREAM_CAPTURE, (uint32 *)&samplerate, 2) == FALSE)
	{
		log_print(LOG_ERR, "open device (%s) failed\r\n", m_alsa.devname);
		return FALSE;
	}
	
	AudioEncoderParam params;
	params.SrcChannels = 2;
	params.SrcSamplefmt = AV_SAMPLE_FMT_S16;
	params.SrcSamplerate = samplerate;
	params.DstChannels = channels;
	params.DstSamplefmt = AV_SAMPLE_FMT_S16;
	params.DstSamplerate = samplerate;
	params.DstBitrate = bitrate;
	params.DstCodec = codec;

	if (m_encoder.init(&params) == FALSE)
	{
		return FALSE;
	}

    m_nChannels = channels;
	m_nSampleRate = samplerate;
	m_nBitrate = bitrate;
	m_bInited = TRUE;
	
	return TRUE;
}

BOOL CLAudioCapture::capture(int *samples)
{
	int res = 0;

	if (NULL == m_alsa.handler)
	{
		return FALSE;
	}

	uint8 data[ALSA_BUFFER_SIZE_MAX];
	
	while (m_bCapture && (res = snd_pcm_readi(m_alsa.handler, data, m_alsa.period_size)) < 0) 
	{
        if (res == -EAGAIN) 
        {
            return FALSE;
        }
        
        if (alsa_xrun_recover(m_alsa.handler, res) < 0) 
        {
            return FALSE;
        }
    }
    
	*samples = res;
	
	return m_encoder.encode(data, res * m_alsa.framesize);
}

void CLAudioCapture::captureThread()
{
	int samples = 0;
	
	while (m_bCapture)
	{
		if (capture(&samples))
		{
			usleep(1000000 * samples / m_nSampleRate);
		}
		else
		{
			usleep(10*1000);
		}
	}

	m_hCapture = 0;
}

BOOL CLAudioCapture::startCapture()
{
    CLock lock(m_pMutex);

	if (!m_bInited)
	{
		return FALSE;
	}
	
	if (m_bCapture)
	{
		return TRUE;
	}
	
	m_bCapture = TRUE;
	m_hCapture = sys_os_create_thread((void *)audioCaptureThread, this);

	return TRUE;
}

void CLAudioCapture::stopCapture(void)
{
	CLock lock(m_pMutex);
	
	m_bCapture = FALSE;
	
	while (m_hCapture)
	{
		usleep(10*1000);
	}

	if (m_alsa.handler)
	{
		snd_pcm_close(m_alsa.handler);
		m_alsa.handler = NULL;
	}

	m_bInited = FALSE;
}







