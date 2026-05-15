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
#include "screen_capture.h"
#include "media_format.h"
#include "lock.h"

CScreenCapture * CScreenCapture::m_pInstance[] = {NULL, NULL, NULL, NULL};
void * CScreenCapture::m_pInstMutex = sys_os_create_mutex();


CScreenCapture::CScreenCapture()
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
}

CScreenCapture::~CScreenCapture()
{
	sys_os_destroy_sig_mutex(m_pMutex);
}

CScreenCapture * CScreenCapture::getInstance(int devid)
{
	return NULL;
}

void CScreenCapture::freeInstance(int devid)
{
	if (devid < 0 || devid >= MAX_MONITOR_NUMS)
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

int CScreenCapture::getDeviceNums()
{
	return 0;
}

BOOL CScreenCapture::initCapture(int codec, int width, int height, int framerate, int bitrate)
{
	return FALSE;
}

char * CScreenCapture::getAuxSDPLine(int rtp_pt)
{
	return m_encoder.getAuxSDPLine(rtp_pt);
}

BOOL CScreenCapture::startCapture()
{
	return FALSE;
}

void CScreenCapture::stopCapture()
{
	
}

BOOL CScreenCapture::capture()
{
	return FALSE;
}

void CScreenCapture::addCallback(VideoDataCallback pCallback, void * pUserdata)
{
	m_encoder.addCallback(pCallback, pUserdata);
}

void CScreenCapture::delCallback(VideoDataCallback pCallback, void * pUserdata)
{
	m_encoder.delCallback(pCallback, pUserdata);
}



