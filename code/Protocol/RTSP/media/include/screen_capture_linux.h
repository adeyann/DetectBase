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

#ifndef _SCREEN_CAPTURE_LINUX_H
#define _SCREEN_CAPTURE_LINUX_H

#include "screen_capture.h"


class CLScreenCapture : public CScreenCapture
{
public:
    // get monitor numbers
    static int getDeviceNums();
    
    static CScreenCapture * getInstance(int devid);
	
	BOOL    initCapture(int codec, int width, int height, int framerate, int bitrate);
	BOOL    startCapture();
    
    void    captureThread();
    
private:
    CLScreenCapture();
    CLScreenCapture(CLScreenCapture &obj);
    ~CLScreenCapture();

	BOOL    capture();
	void    stopCapture();
	
private:	
	int         m_fd;
	uint8     * m_data;     // framebuffer data
	int         m_smem_len;
	int         m_linesize;
    int         m_framesize;
    int         m_bytes_per_pixel;
    int         m_line_length;
};

#endif



