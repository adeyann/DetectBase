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

#ifndef _SCREEN_CAPTURE_H
#define _SCREEN_CAPTURE_H

#include "video_encoder.h"

/***************************************************************************************/

#define MAX_MONITOR_NUMS  4

/***************************************************************************************/

class CScreenCapture
{
public:
    virtual ~CScreenCapture();

    // get monitor numbers
    static int      getDeviceNums();
    
    static CScreenCapture * getInstance(int devid);
    
	virtual void    freeInstance(int devid);
	
	virtual BOOL    initCapture(int codec, int width, int height, int framerate, int bitrate);
	virtual BOOL    startCapture();

    virtual void    addCallback(VideoDataCallback pCallback, void * pUserdata);
    virtual void    delCallback(VideoDataCallback pCallback, void * pUserdata);

    virtual int     getWidth() {return m_nWidth;}
    virtual int     getHeight() {return m_nHeight;}
    virtual char *  getAuxSDPLine(int rtp_pt);
    
protected:
    CScreenCapture();
    CScreenCapture(CScreenCapture &obj);

	virtual BOOL    capture();
	virtual void    stopCapture();

public:
    int                     m_nDevIndex;
    int                     m_nRefCnt;

    static void *           m_pInstMutex;
	static CScreenCapture*  m_pInstance[MAX_MONITOR_NUMS];
	
protected:
	int                     m_nWidth;
	int	                    m_nHeight;
	int                     m_nFramerate;
	int                     m_nBitrate;

    CVideoEncoder           m_encoder;

    void *                  m_pMutex;
    BOOL                    m_bInited;
    BOOL                    m_bCapture;
    pthread_t               m_hCapture;
};

#endif



