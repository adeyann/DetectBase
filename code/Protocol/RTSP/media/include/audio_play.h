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

#ifndef AUDIO_PLAY_H
#define AUDIO_PLAY_H

#include "sys_inc.h"
#include "media_format.h"


class CAudioPlay
{
public:
	CAudioPlay(int samplerate, int channels);
	virtual ~CAudioPlay();
	
public:
	virtual BOOL    startPlay();
	virtual void    stopPlay();
	virtual int     setVolume(int volume);
	virtual int     getVolume();
	virtual void    playAudio(uint8 * pData, int len);

protected:
	BOOL    m_bInited;
	int     m_nSamplerate;
	int     m_nChannels;
};

#endif // AUDIO_PLAY_H



