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
#include "video_capture_linux.h"
#include "media_format.h"
#include "lock.h"
#include "v4l2.h"
#include "v4l2_comm.h"
#include <linux/videodev2.h>


/***************************************************************************************/

static void * videoCaptureThread(void * argv)
{
	CLVideoCapture *capture = (CLVideoCapture *)argv;

	capture->captureThread();

	return NULL;
}

CLVideoCapture::CLVideoCapture(void) : CVideoCapture()
{
	m_fd = 0;
	m_nBuffers = 0;
	m_pBufStart = NULL;
	m_pBufLen = NULL;
}

CLVideoCapture::~CLVideoCapture(void)
{
    stopCapture();
}

CVideoCapture * CLVideoCapture::getInstance(int devid)
{
	if (devid < 0 || devid >= MAX_VIDEO_DEV_NUMS)
	{
		return NULL;
	}
	
	if (NULL == m_pInstance[devid])
	{
		sys_os_mutex_enter(m_pInstMutex);

		if (NULL == m_pInstance[devid])
		{
			m_pInstance[devid] = (CVideoCapture*) new CLVideoCapture;
			if (m_pInstance[devid])
			{
				m_pInstance[devid]->m_nRefCnt++;
				m_pInstance[devid]->m_nDevIndex = devid;
			}
		}
		
		sys_os_mutex_leave(m_pInstMutex);
	}
	else
	{
		sys_os_mutex_enter(m_pInstMutex);
		m_pInstance[devid]->m_nRefCnt++;
		sys_os_mutex_leave(m_pInstMutex);
	}

	return m_pInstance[devid];
}

int CLVideoCapture::getDeviceNums()
{
	int count = 0;
	DIR *dir;
	struct dirent *entry;
	char filename[1024] = {'\0'};
	int fd;

	dir = opendir("/dev");
	if (!dir) 
	{
		log_print(LOG_ERR, "%s, opendir failed\r\n", __FUNCTION__);
		return 0;
	}

	while ((entry = readdir(dir))) 
	{
		if (strncmp(entry->d_name, "video", 5))
		{
			continue;
		}

        snprintf(filename, sizeof(filename), "/dev/%s", entry->d_name);
        if ((fd = v4l2_open_device(filename)) < 0)
        {
            continue;
		}

		count++;
        close(fd);
    }
    
    closedir(dir);
	
	return count;
}

BOOL CLVideoCapture::getDeviceName(int index, char * name, int len)
{
	BOOL ret = FALSE;
	int count = 0;
	DIR *dir;
	struct dirent *entry;
	char filename[1024] = {'\0'};
	int fd;

	dir = opendir("/dev");
	if (!dir) 
	{
		log_print(LOG_ERR, "%s, opendir failed\r\n", __FUNCTION__);
		return FALSE;
	}

	while ((entry = readdir(dir))) 
	{
		if (strncmp(entry->d_name, "video", 5))
		{
			continue;
		}

        snprintf(filename, sizeof(filename), "/dev/%s", entry->d_name);
        if ((fd = v4l2_open_device(filename)) < 0)
        {
            continue;
		}

		close(fd);
		
		if (count == index)
		{
			ret = TRUE;
			strncpy(name, filename, len);
			break;
		}
		
		count++;        
    }
    
    closedir(dir);
	
	return ret;
}

BOOL CLVideoCapture::initCapture(int codec, int width, int height, int framerate, int bitrate)
{
	int ret;
	char filename[1024] = {'\0'};
	uint32 desired_format;

	CLock lock(m_pMutex);
	
	if (m_bInited)
	{
		return TRUE;
	}

	m_nFramerate = framerate;

	if (!getDeviceName(m_nDevIndex, filename, sizeof(filename)))
	{
		return FALSE;
	}
	
	m_fd = v4l2_open_device(filename);
	if (m_fd < 0)
	{
		return FALSE;
	}	

	if (!m_nWidth && !m_nHeight) 
	{
        struct v4l2_format fmt;
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		
        log_print(LOG_DBG, "Querying the device for the current frame size\r\n");
        
        if (ioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0) 
        {
            log_print(LOG_ERR, "ioctl(VIDIOC_G_FMT) failed\r\n");
            return FALSE;
        }

        m_nWidth  = fmt.fmt.pix.width;
        m_nHeight = fmt.fmt.pix.height;
        log_print(LOG_DBG, "Setting frame size to %dx%d\r\n", m_nWidth, m_nHeight);
    }

	ret = initDevice(&m_nWidth, &m_nHeight, &desired_format);
    if (ret < 0)
    {
        return FALSE;
	}
	
	m_nVideoFormat = toLocalVideoFormat(desired_format);	

	ret = mmapInit();
	if (ret < 0)
    {
        return FALSE;
	}

	VideoEncoderParam params;
	memset(&params, 0, sizeof(params));
	
	params.SrcWidth = m_nWidth;
	params.SrcHeight = m_nHeight;
	params.SrcPixFmt = CVideoEncoder::toAVPixelFormat(m_nVideoFormat);
	params.DstCodec = codec;
	params.DstWidth = width ? width : m_nWidth;
	params.DstHeight = height ? height : m_nHeight;
	params.DstFramerate = framerate;
	params.DstBitrate = bitrate;
		
	if (m_encoder.init(&params) == FALSE)
	{
		return FALSE;
	}

	m_nBitrate = bitrate;
   	m_bInited = TRUE;
   	
	return TRUE;
}

BOOL CLVideoCapture::capture()
{
	BOOL res = FALSE;
	int ret;
    struct v4l2_buffer buf;

	if (!m_bInited)
	{
		return FALSE;
	}

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	
	ret = ioctl(m_fd, VIDIOC_DQBUF, &buf);
	if (ret < 0) 
	{
		log_print(LOG_ERR, "VIDIOC_DQBUF failed (%d)\r\n", ret);
		return FALSE;
	}

	if (static_cast<int>(buf.index) >= m_nBuffers) 
	{
        log_print(LOG_ERR, "Invalid buffer index received.\r\n");
        return FALSE;
    }

	if (!m_encoder.encode((uint8 *)m_pBufStart[buf.index], buf.length))
	{
		//log_print(LOG_ERR, "encode failed\r\n");
		res = FALSE;
	}
	
	// re-queen buffer
	ret = ioctl(m_fd, VIDIOC_QBUF, &buf);
	if (ret < 0) 
	{
		log_print(LOG_ERR, "VIDIOC_QBUF failed (%d)\r\n", ret);
		return FALSE;
	}

	return res;	
}

BOOL CLVideoCapture::startCapture()
{
	CLock lock(m_pMutex);
	
	if (m_hCapture)
	{
		return TRUE;
	}
	
	m_bCapture = TRUE;
	m_hCapture = sys_os_create_thread((void *)videoCaptureThread, this);

	return (m_hCapture ? TRUE : FALSE);
}

void CLVideoCapture::stopCapture()
{
	CLock lock(m_pMutex);
	
	// wiat for capture thread exit
	m_bCapture = FALSE;
	
	while (m_hCapture)
	{
		usleep(10*1000);
	}

	mmapUninit();

    uninitDevice();

    m_bInited = FALSE;
}

void CLVideoCapture::captureThread()
{	
	uint32 curtime;
	uint32 lasttime = sys_os_get_ms();
	double timestep = 1000.0 / m_nFramerate;
		
	while (m_bCapture)
	{
		if (capture())
		{
			curtime = sys_os_get_ms();
			if (curtime - lasttime < timestep)
			{
				usleep(timestep * 1000);
			}
			else
			{
				usleep(10*1000);
			}

			lasttime = curtime;
		}
		else
		{
			usleep(10*1000);
		}
	}

	m_hCapture = 0;
}


/****************************************************************************/
int CLVideoCapture::initDevice(int *width, int *height, uint32 *desired_format)
{
	int ret = -1;
	int i;

    for (i = 0; ff_fmt_conversion_table[i].codec_id != AV_CODEC_ID_NONE; i++) 
    {
        *desired_format = ff_fmt_conversion_table[i].v4l2_fmt;
        ret = v4l2_init_device(m_fd, width, height, *desired_format);
        if (ret >= 0)
        {
            break;
        }
        
        *desired_format = 0;
    }

    if (*desired_format == 0) 
    {
        log_print(LOG_ERR, "Cannot find a proper format\r\n");
        ret = -1;
    }
    
    return ret;
}

void CLVideoCapture::uninitDevice()
{
	close(m_fd);
	m_fd = 0;
}

int CLVideoCapture::mmapInit()
{	
	int i, ret;
	struct v4l2_requestbuffers reqbuf;
		
	reqbuf.count = QBUF_COUNT;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	
	ret = ioctl(m_fd , VIDIOC_REQBUFS, &reqbuf);
	if (ret < 0) 
	{
		log_print(LOG_ERR, "ioctl(VIDIOC_REQBUFS failed) %d\r\n", ret);
		return ret;
	}

	m_nBuffers = reqbuf.count;
	m_pBufStart = (void **) malloc(m_nBuffers * sizeof(void *));
	if (NULL == m_pBufStart)
	{
		log_print(LOG_ERR, "malloc failed\r\n", ret);
		return -1;
	}
	
	m_pBufLen = (uint32 *) malloc(m_nBuffers * sizeof(uint32));
	if (NULL == m_pBufLen)
	{
		log_print(LOG_ERR, "malloc failed\r\n", ret);
		return -1;
	}
	
	struct v4l2_buffer buf;

	int range = static_cast<int>( reqbuf.count );
	for (i = 0; i < range; i++)
	{
		buf.index = i;
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		ret = ioctl(m_fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) 
		{
			log_print(LOG_ERR, "ioctl(VIDIOC_QUERYBUF failed) %d\r\n", ret);
			return ret;
		}
		
		// mmap buffer
		m_pBufLen[i] = buf.length;
		m_pBufStart[i] = (void *) mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);

		if (m_pBufStart[i] == MAP_FAILED) 
		{
			log_print(LOG_ERR, "mmap (%d) failed: %s\r\n", i, strerror(errno));
			return -1;
		}
	
		// queen buffer
		ret = ioctl(m_fd , VIDIOC_QBUF, &buf);
		if (ret < 0) 
		{
			log_print(LOG_ERR, "VIDIOC_QBUF (%d) failed (%d)\r\n", i, ret);
			return ret;
		}

		log_print(H_LOG_INFO, "buffer %d: addr=0x%ul, len=%d\r\n", i, (unsigned long)m_pBufStart[i], m_pBufLen[i]);
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(m_fd, VIDIOC_STREAMON, &type);
	if (ret < 0) 
	{
		log_print(LOG_ERR, "VIDIOC_STREAMON failed (%d)\r\n", ret);
		return ret;
	}
	
	return 0;
}

void CLVideoCapture::mmapUninit()
{
	int i;
	enum v4l2_buf_type type;    

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* We do not check for the result, because we could
     * not do anything about it anyway...
     */
    ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    
    for (i = 0; i < m_nBuffers; i++) 
    {
        munmap(m_pBufStart[i], m_pBufLen[i]);
    }

    if (m_pBufStart)
    {
    	free(m_pBufStart);
    }

    if (m_pBufLen)
    {
    	free(m_pBufLen);
    }

    m_pBufStart = NULL;
    m_pBufLen = NULL;
}

int CLVideoCapture::toLocalVideoFormat(int v4l2_pixfmt)
{
	int pixfmt = VIDEO_FMT_NONE;
	
	switch (v4l2_pixfmt)
	{
	case V4L2_PIX_FMT_YUYV:
		pixfmt = VIDEO_FMT_YUYV422;
		break;

	case V4L2_PIX_FMT_YUV420:
		pixfmt = VIDEO_FMT_YUV420P;
		break;

	case V4L2_PIX_FMT_UYVY:
		pixfmt = VIDEO_FMT_UYVY422;
		break;	

	case V4L2_PIX_FMT_RGB24:
		pixfmt = VIDEO_FMT_RGB24;
		break;

	case V4L2_PIX_FMT_NV12:
		pixfmt = VIDEO_FMT_NV12;
		break;
	}

	return pixfmt;
}


