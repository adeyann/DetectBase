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

#ifndef LIVE_AUDIO_H
#define LIVE_AUDIO_H

/***************************************************************************************/

#include "linked_list.h"

/***************************************************************************************/

#define MAX_LIVE_AUDIO_NUMS  4

typedef void (*LiveAudioDataCB)(uint8 *data, int size, int nbsamples, void *pUserdata);

typedef struct
{
	LiveAudioDataCB pCallback;    // callback function pointer
	void *          pUserdata;    // user data
} LiveAudioCB;

/***************************************************************************************/

class CLiveAudio
{
public:
    virtual ~CLiveAudio();
    
    // get audio capture devcie nubmers     
    static int      getDeviceNums();

    // get single instance
	static CLiveAudio * getInstance(int devid);
	
	// free instance
	virtual void    freeInstance(int devid);

    virtual BOOL    initCapture(int codec, int sampleRate, int channels, int bitrate);
	virtual BOOL    startCapture();
    
    virtual char *  getAuxSDPLine(int rtp_pt);
    
    virtual void    addCallback(LiveAudioDataCB pCallback, void * pUserdata);
    virtual void    delCallback(LiveAudioDataCB pCallback, void * pUserdata);

    virtual BOOL    captureThread();    
    
protected:
    CLiveAudio();
    CLiveAudio(CLiveAudio &obj);	
	
    virtual void    stopCapture();
    
    BOOL            isCallbackExist(LiveAudioDataCB pCallback, void *pUserdata);
    void            procData(uint8 * data, int size, int nbsamples);
    
public:
    int                     m_nDevIndex;
    int                     m_nRefCnt;      // single instance ref count

    static void *           m_pInstMutex;   // instance mutex
    static CLiveAudio *     m_pInstance[MAX_LIVE_AUDIO_NUMS];
    
protected:    
    BOOL			        m_bCapture;     // capturing flag
    pthread_t               m_hCapture;
    
    int                     m_nSampleRate;  // sample rate
    int                     m_nChannel;     // channel
    int                     m_nBitrate;     // bitrate
    
    void *                  m_pMutex;       // mutex
    BOOL                    m_bInited;      // inited flag

    void *                  m_pCallbackMutex;
	LINKED_LIST *           m_pCallbackList;
};


#endif

