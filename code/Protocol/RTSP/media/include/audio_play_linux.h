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

#ifndef AUDIO_PLAY_LINUX_H
#define AUDIO_PLAY_LINUX_H

#include "sys_inc.h"
#include "alsa.h"
#include "audio_play.h"


class CLAudioPlay : public CAudioPlay
{
public:
	CLAudioPlay(int samplerate, int channels);
	~CLAudioPlay();
	
public:
	BOOL    startPlay();
	void    stopPlay();
	int     setVolume(int volume);
	int     getVolume();
	void    playAudio(uint8 * pData, int len);
	

private:
	ALSACTX     m_alsa;
	
};

#endif // AUDIO_PLAY_LINUX_H



