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

#ifndef LIVE_VIDEO_H
#define LIVE_VIDEO_H

#include "linked_list.h"

/***************************************************************************************/

#define MAX_STREAM_NUMS  4

typedef void (*LiveVideoDataCB)(uint8 *data, int size, void *pUserdata);

typedef struct
{
	LiveVideoDataCB pCallback;
	void *          pUserdata;
	BOOL            bFirst;
} LiveVideoCB;

/***************************************************************************************/


class CLiveVideo
{
public:
    virtual ~CLiveVideo();
    
    // get the support stream numbers
    static int      getStreamNums();
    
    static CLiveVideo * getInstance(int idx);
    
    virtual void    freeInstance(int idx);

    virtual BOOL    initCapture(int codec, int width, int height, int framerate, int bitrate);
	virtual BOOL    startCapture();

    virtual void    addCallback(LiveVideoDataCB pCallback, void * pUserdata);
    virtual void    delCallback(LiveVideoDataCB pCallback, void * pUserdata);
    
    virtual char *  getAuxSDPLine(int rtp_pt);
    virtual BOOL    captureThread();
    
protected:
    CLiveVideo();
    CLiveVideo(CLiveVideo &obj);

    BOOL            isCallbackExist(LiveVideoDataCB pCallback, void *pUserdata);
    void            procData(uint8 * data, int size);
    
	virtual void    stopCapture();

public:
    int                     m_nDevIndex;
    int                     m_nRefCnt;

    static void *           m_pInstMutex;
	static CLiveVideo *     m_pInstance[MAX_STREAM_NUMS];
	
protected:
	int                     m_nWidth;
	int	                    m_nHeight;
	int                     m_nFramerate;
    int                     m_nBitrate;
    
    void *                  m_pMutex;
    BOOL                    m_bInited;
    BOOL                    m_bCapture;
    pthread_t               m_hCapture;

    void *                  m_pCallbackMutex;
	LINKED_LIST *           m_pCallbackList;
};


#endif



