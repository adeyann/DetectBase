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

#ifndef _AUDIO_CAPTURE_LINUX_H
#define _AUDIO_CAPTURE_LINUX_H

/***************************************************************************************/

#include "audio_capture.h"
#include <alsa/asoundlib.h>
#include "alsa.h"

/***************************************************************************************/

class CLAudioCapture : public CAudioCapture
{
public:
    
    // get audio capture devcie nubmers     
    static int getDeviceNums();

    // get single instance
	static CAudioCapture * getInstance(int devid);

    BOOL    initCapture(int codec, int sampleRate, int channels, int bitrate);
	BOOL    startCapture();

    void    captureThread();
    
private:
    CLAudioCapture();
    CLAudioCapture(CLAudioCapture &obj);
	~CLAudioCapture();
	
    void    stopCapture(); 
    BOOL    capture(int *samples);
    
private:
    ALSACTX     m_alsa;    
    pthread_t   m_hCapture;
};


#endif // _AUDIO_CAPTURE_LINUX_H


