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
#include "audio_play.h"


CAudioPlay::CAudioPlay(int samplerate, int channels)
{
	m_bInited = FALSE;
	m_nSamplerate = samplerate;
	m_nChannels = channels;
}

CAudioPlay::~CAudioPlay(void)
{
}

BOOL CAudioPlay::startPlay()
{
	return FALSE;
}

void CAudioPlay::stopPlay(void)
{
	m_bInited = FALSE;
}


int CAudioPlay::setVolume(int volume)
{
    return 0;
}


int CAudioPlay::getVolume()
{    
    return 0;
}

	
void CAudioPlay::playAudio(uint8 * data, int size) 
{ 
	if (!m_bInited)
	{
		return;
	}
}




