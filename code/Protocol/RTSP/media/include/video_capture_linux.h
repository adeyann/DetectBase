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

#ifndef _VIDIO_CAPTURE_LINUX_H
#define _VIDIO_CAPTURE_LINUX_H

#include "video_capture.h"

/*************************************************************************************/
#define QBUF_COUNT          4   

/*************************************************************************************/

class CLVideoCapture : public CVideoCapture
{
public:
    // get video capture devices numbers
    static int getDeviceNums();

    // get the single instance
	static CVideoCapture * getInstance(int devid);

	BOOL    initCapture(int codec, int width, int height, int framerate, int bitrate);
	BOOL    startCapture();    

    void    captureThread();
    
private:
    CLVideoCapture();
    CLVideoCapture(CLVideoCapture &obj);
	~CLVideoCapture();

    BOOL    capture();
	void    stopCapture();
	int     initDevice(int *width, int *height, uint32 *desired_format);
	void    uninitDevice();
	int     mmapInit();
	void    mmapUninit();
	int     toLocalVideoFormat(int v4l2_pixfmt);
    BOOL    getDeviceName(int index, char * name, int len);
    
private:
    
    int         m_fd;
    int         m_nBuffers;
    void **     m_pBufStart;
    uint32 *    m_pBufLen;
};

#endif // _VIDIO_CAPTURE_LINUX_H


