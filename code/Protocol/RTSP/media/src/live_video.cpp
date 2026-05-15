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
#include "live_video.h"
#include "media_format.h"
#include "lock.h"

#ifdef RTSP_LIVE

/**************************************************************************************/

CLiveVideo * CLiveVideo::m_pInstance[] = {NULL, NULL, NULL, NULL};
void * CLiveVideo::m_pInstMutex = sys_os_create_mutex();


/**************************************************************************************/

void * liveVideoThread(void * argv)
{
	CLiveVideo *capture = (CLiveVideo *)argv;

	capture->captureThread();

	return NULL;
}

/**************************************************************************************/

CLiveVideo::CLiveVideo()
{
	m_nDevIndex = 0;	
	m_nWidth = 0;
	m_nHeight = 0;
	m_nFramerate = 15;
	m_nBitrate = 0;

	m_pMutex = sys_os_create_mutex();
	
	m_bInited = FALSE;
	m_bCapture = FALSE;
	m_hCapture = 0;
	
	m_nRefCnt = 0;

	m_pCallbackMutex = sys_os_create_mutex();
    m_pCallbackList = h_list_create(FALSE);
}

CLiveVideo::~CLiveVideo()
{
	stopCapture();
	
	sys_os_destroy_sig_mutex(m_pMutex);

	h_list_free_container(m_pCallbackList);
	
	sys_os_destroy_sig_mutex(m_pCallbackMutex);
}

CLiveVideo * CLiveVideo::getInstance(int idx)
{
	if (idx < 0 || idx >= MAX_STREAM_NUMS)
	{
		return NULL;
	}
	
	if (NULL == m_pInstance[idx])
	{
		sys_os_mutex_enter(m_pInstMutex);

		if (NULL == m_pInstance[idx])
		{
			m_pInstance[idx] = (CLiveVideo *) new CLiveVideo;
			if (m_pInstance[idx])
			{
				m_pInstance[idx]->m_nRefCnt++;
				m_pInstance[idx]->m_nDevIndex = idx;
			}
		}
		
		sys_os_mutex_leave(m_pInstMutex);
	}
	else
	{
		sys_os_mutex_enter(m_pInstMutex);
		m_pInstance[idx]->m_nRefCnt++;
		sys_os_mutex_leave(m_pInstMutex);
	}

	return m_pInstance[idx];
}

void CLiveVideo::freeInstance(int idx)
{
	if (idx < 0 || idx >= MAX_STREAM_NUMS)
	{
		return;
	}
	
	sys_os_mutex_enter(m_pInstMutex);
	
	if (m_pInstance[idx])
	{
		m_pInstance[idx]->m_nRefCnt--;

		if (m_pInstance[idx]->m_nRefCnt <= 0)
		{
			delete m_pInstance[idx];
			m_pInstance[idx] = NULL;
		}
	}

	sys_os_mutex_leave(m_pInstMutex);
}

int CLiveVideo::getStreamNums()
{
	// todo : return the max number of streams supported, don't be more than MAX_STREAM_NUMS

	
	return 1;
}

BOOL CLiveVideo::initCapture(int codec, int width, int height, int framerate, int bitrate)
{
	CLock lock(m_pMutex);
	
	if (m_bInited)
	{
		return TRUE;
	}

	m_nWidth = width;
	m_nHeight= height;
	m_nFramerate = framerate;
	m_nBitrate = bitrate;

	// todo : here add your init code ... 
	

	m_bInited = TRUE;

	return TRUE;
}

char * CLiveVideo::getAuxSDPLine(int rtp_pt)
{
	return NULL;
}

BOOL CLiveVideo::startCapture()
{
	CLock lock(m_pMutex);
	
	if (m_hCapture)
	{
		return TRUE;
	}

	m_bCapture = TRUE;
	m_hCapture = sys_os_create_thread((void *)liveVideoThread, this);

	return m_hCapture ? TRUE : FALSE;
}

void CLiveVideo::stopCapture()
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

BOOL CLiveVideo::captureThread()
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

BOOL CLiveVideo::isCallbackExist(LiveVideoDataCB pCallback, void *pUserdata)
{
	BOOL exist = FALSE;
	LiveVideoCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveVideoCB *) p_node->p_data;
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

void CLiveVideo::addCallback(LiveVideoDataCB pCallback, void * pUserdata)
{
	if (isCallbackExist(pCallback, pUserdata))
	{
		return;
	}
	
	LiveVideoCB * p_cb = (LiveVideoCB *) malloc(sizeof(LiveVideoCB));

	p_cb->pCallback = pCallback;
	p_cb->pUserdata = pUserdata;
	p_cb->bFirst = TRUE;

	sys_os_mutex_enter(m_pCallbackMutex);
	h_list_add_at_back(m_pCallbackList, p_cb);	
	sys_os_mutex_leave(m_pCallbackMutex);
}

void CLiveVideo::delCallback(LiveVideoDataCB pCallback, void * pUserdata)
{
	LiveVideoCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveVideoCB *) p_node->p_data;
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

void CLiveVideo::procData(uint8 * data, int size)
{
	LiveVideoCB * p_cb = NULL;
	LINKED_NODE * p_node = NULL;
	
	sys_os_mutex_enter(m_pCallbackMutex);

	p_node = h_list_lookup_start(m_pCallbackList);
	while (p_node)
	{
		p_cb = (LiveVideoCB *) p_node->p_data;
		if (p_cb->pCallback != NULL)
		{		    
			p_cb->pCallback(data, size, p_cb->pUserdata);
		}
		
		p_node = h_list_lookup_next(m_pCallbackList, p_node);
	}
	h_list_lookup_end(m_pCallbackList);

	sys_os_mutex_leave(m_pCallbackMutex);
}


#endif // RTSP_LIVE


