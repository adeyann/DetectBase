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
#include "live_audio.h"
#include "lock.h"

#ifdef RTSP_LIVE

/***************************************************************************************/

CLiveAudio * CLiveAudio::m_pInstance[] = {NULL, NULL, NULL, NULL};
void * CLiveAudio::m_pInstMutex = sys_os_create_mutex();


/***************************************************************************************/

void * liveAudioThread(void * argv)
{
	CLiveAudio *capture = (CLiveAudio *)argv;

	capture->captureThread();

	return NULL;
}

/***************************************************************************************/

CLiveAudio::CLiveAudio()
{ 
    m_nDevIndex = 0;
    m_bCapture = FALSE;
    m_hCapture = 0;
	m_nSampleRate = 8000;
	m_nBitrate = 0;
	
	m_pMutex = sys_os_create_mutex();	
	m_bInited = FALSE;	
	m_nRefCnt = 0;  

	m_pCallbackMutex = sys_os_create_mutex();
    m_pCallbackList = h_list_create(FALSE);
}

CLiveAudio::~CLiveAudio()
{ 
	stopCapture();
	
	sys_os_destroy_sig_mutex(m_pMutex);

	h_list_free_container(m_pCallbackList);
	
	sys_os_destroy_sig_mutex(m_pCallbackMutex);
}

CLiveAudio * CLiveAudio::getInstance(int devid)
{
	if (devid < 0 || devid >= MAX_LIVE_AUDIO_NUMS)
	{
		return NULL;
	}
	
	if (NULL == m_pInstance[devid])
	{
		sys_os_mutex_enter(m_pInstMutex);

		if (NULL == m_pInstance[devid])
		{
			m_pInstance[devid] = new CLiveAudio;
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

void CLiveAudio::freeInstance(int devid)
{
	if (devid < 0 || devid >= MAX_LIVE_AUDIO_NUMS)
	{
		return;
	}
	
	sys_os_mutex_enter(m_pInstMutex);
	
	if (m_pInstance[devid])
	{
		m_pInstance[devid]->m_nRefCnt--;

		if (m_pInstance[devid]->m_nRefCnt <= 0)
		{
			delete m_pInstance[devid];
			m_pInstance[devid] = NULL;
		}
	}

	sys_os_mutex_leave(m_pInstMutex);
}

char * CLiveAudio::getAuxSDPLine(int rtp_pt)
{
	return NULL;
}

int CLiveAudio::getDeviceNums()
{
	// todo : return the max number of streams supported, don't be more than MAX_LIVE_AUDIO_NUMS
	
    return 1;
}

BOOL CLiveAudio::initCapture(int codec, int samplerate, int channels, int bitrate)
{
	CLock lock(m_pMutex);
	
	if (m_bInited)
	{
		return TRUE;
	}

	m_nSampleRate = samplerate;
	m_nChannel = channels;
	m_nBitrate = bitrate;

	// todo : here add your init code ... 
	

	m_bInited = TRUE;

	return TRUE;
}

BOOL CLiveAudio::startCapture()
{
    CLock lock(m_pMutex);
	
	if (m_hCapture)
	{
		return TRUE;
	}

	m_bCapture = TRUE;
	m_hCapture = sys_os_create_thread((void *)liveAudioThread, this);

	return m_hCapture ? TRUE : FALSE;
}

void CLiveAudio::stopCapture(void)
{
	CLock lock(m_pMutex);
	
	m_bCapture = FALSE;
	
	while (m_hCapture)
	{
		usleep(10*1000);
	}

	// todo : here add your uninit code ...

	

	m_bInited = FALSE;
}

BOOL CLiveAudio::captureThread()
{
	// todo : when get the encoded data, call procData 

	while (m_bCapture)
	{
        // todo : here add the capture handler ... 


        usleep(10*1000);
	}

	m_hCapture = 0;
	
	return TRUE;
}

BOOL CLiveAudio::isCallbackExist(LiveAudioDataCB pCallback, void *pUserdata)
{
	BOOL exist = FALSE;
	LiveAudioCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveAudioCB *) p_node->p_data;
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

void CLiveAudio::addCallback(LiveAudioDataCB pCallback, void * pUserdata)
{
	if (isCallbackExist(pCallback, pUserdata))
	{
		return;
	}
	
	LiveAudioCB * p_cb = (LiveAudioCB *) malloc(sizeof(LiveAudioCB));

	p_cb->pCallback = pCallback;
	p_cb->pUserdata = pUserdata;

	sys_os_mutex_enter(m_pCallbackMutex);
	h_list_add_at_back(m_pCallbackList, p_cb);	
	sys_os_mutex_leave(m_pCallbackMutex);
}

void CLiveAudio::delCallback(LiveAudioDataCB pCallback, void * pUserdata)
{
	LiveAudioCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveAudioCB *) p_node->p_data;
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

void CLiveAudio::procData(uint8 * data, int size, int nbsamples)
{
	LiveAudioCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveAudioCB *) p_node->p_data;
		if (p_cb->pCallback != NULL)
		{ 
			p_cb->pCallback(data, size, nbsamples, p_cb->pUserdata);
		}
		
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}

#endif // end of RTSP_LIVE


