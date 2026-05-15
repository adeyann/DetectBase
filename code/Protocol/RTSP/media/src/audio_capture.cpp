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
#include "audio_capture.h"
#include "lock.h"

/***************************************************************************************/

CAudioCapture * CAudioCapture::m_pInstance[] = {NULL, NULL, NULL, NULL};
void * CAudioCapture::m_pInstMutex = sys_os_create_mutex();

/***************************************************************************************/

CAudioCapture::CAudioCapture()
{ 
    m_nDevIndex = 0;
    m_bCapture = FALSE;
    m_nChannels = 2;
	m_nSampleRate = 8000;
	m_nBitrate = 0;
	
	m_pMutex = sys_os_create_mutex();	
	m_bInited = FALSE;	
	m_nRefCnt = 0;  
}

CAudioCapture::~CAudioCapture()
{ 
    sys_os_destroy_sig_mutex(m_pMutex);
}

CAudioCapture * CAudioCapture::getInstance(int devid)
{
	return NULL;
}

void CAudioCapture::freeInstance(int devid)
{
	if (devid < 0 || devid >= MAX_AUDIO_DEV_NUMS)
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

char * CAudioCapture::getAuxSDPLine(int rtp_pt)
{
	return m_encoder.getAuxSDPLine(rtp_pt);
}

int CAudioCapture::getDeviceNums()
{
    return 0;
}

BOOL CAudioCapture::initCapture(int codec, int samplerate, int channels, int bitrate)
{
	return FALSE;
}

BOOL CAudioCapture::startCapture()
{
    return FALSE;
}

void CAudioCapture::stopCapture(void)
{
	
}

void CAudioCapture::addCallback(AudioDataCallback pCallback, void * pUserdata)
{
	m_encoder.addCallback(pCallback, pUserdata);
}

void CAudioCapture::delCallback(AudioDataCallback pCallback, void * pUserdata)
{
	m_encoder.delCallback(pCallback, pUserdata);
}



