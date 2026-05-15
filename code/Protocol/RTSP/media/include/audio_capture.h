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

#ifndef _AUDIO_CAPTURE_H
#define _AUDIO_CAPTURE_H

/***************************************************************************************/

#include "audio_encoder.h"

/***************************************************************************************/

#define MAX_AUDIO_DEV_NUMS  4

/***************************************************************************************/

class CAudioCapture
{
public:
    virtual ~CAudioCapture();
    
    // get audio capture devcie nubmers     
    static int      getDeviceNums();

    // get single instance
	static CAudioCapture * getInstance(int devid);
	// free instance
	virtual void    freeInstance(int devid);

    virtual BOOL    initCapture(int codec, int sampleRate, int channels, int bitrate);
	virtual BOOL    startCapture();
    
    virtual char *  getAuxSDPLine(int rtp_pt);
    
    virtual void    addCallback(AudioDataCallback pCallback, void * pUserdata);
    virtual void    delCallback(AudioDataCallback pCallback, void * pUserdata);

    virtual BOOL    isCapturing() {return m_bCapture;}
    
protected:
    CAudioCapture();
    CAudioCapture(CAudioCapture &obj);	
	
    virtual void    stopCapture();

public:
    int                     m_nDevIndex;
    int                     m_nRefCnt;      // single instance ref count

    static void *           m_pInstMutex;   // instance mutex
    static CAudioCapture *  m_pInstance[MAX_AUDIO_DEV_NUMS];
    
protected:    
    BOOL			        m_bCapture;   	// capturing flag
    int                     m_nChannels;    // channel number
    int                     m_nSampleRate;  // sample rate
    int                     m_nBitrate;     // bitrate
    
    CAudioEncoder           m_encoder;      // audio encoder

    void *                  m_pMutex;       // mutex
    BOOL                    m_bInited;      // inited flag
};


#endif


