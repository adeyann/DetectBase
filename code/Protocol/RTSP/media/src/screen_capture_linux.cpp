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
#include "screen_capture_linux.h"
#include "media_format.h"
#include "lock.h"
#include <linux/fb.h>


/**************************************************************************************/
struct pixfmt_map_entry 
{
    int bits_per_pixel;
    int red_offset, green_offset, blue_offset, alpha_offset;
    enum AVPixelFormat pixfmt;
};

static const struct pixfmt_map_entry pixfmt_map[] = 
{
    // bpp, red_offset,  green_offset, blue_offset, alpha_offset, pixfmt
    {  32,       0,           8,          16,           24,   AV_PIX_FMT_RGBA  },
    {  32,      16,           8,           0,           24,   AV_PIX_FMT_BGRA  },
    {  32,       8,          16,          24,            0,   AV_PIX_FMT_ARGB  },
    {  32,       3,           2,           8,            0,   AV_PIX_FMT_ABGR  },
    {  24,       0,           8,          16,            0,   AV_PIX_FMT_RGB24 },
    {  24,      16,           8,           0,            0,   AV_PIX_FMT_BGR24 },
    {  16,      11,           5,           0,            0,   AV_PIX_FMT_RGB565},
};

enum AVPixelFormat getPixfmt(struct fb_var_screeninfo *varinfo)
{
    int i;
	int range = static_cast<int>( sizeof( pixfmt_map ) / sizeof( pixfmt_map[0] ) );

    for (i = 0; i < range; i++)
    {
        const struct pixfmt_map_entry *entry = &pixfmt_map[i];
        if (entry->bits_per_pixel == static_cast<int>(varinfo->bits_per_pixel) &&
            entry->red_offset     == static_cast<int>(varinfo->red.offset)     &&
            entry->green_offset   == static_cast<int>(varinfo->green.offset)   &&
            entry->blue_offset    == static_cast<int>(varinfo->blue.offset)    )
        {    
            return entry->pixfmt;
        }    
    }

    return AV_PIX_FMT_NONE;
}

/**************************************************************************************/

void * screenCaptureThread(void * argv)
{
	CLScreenCapture *capture = (CLScreenCapture *)argv;

	capture->captureThread();

	return NULL;
}

/**************************************************************************************/

CLScreenCapture::CLScreenCapture()  : CScreenCapture()
{
	m_fd = 0;
	m_data = NULL;
	m_smem_len = 0;
	m_linesize = 0;
    m_framesize = 0;
    m_bytes_per_pixel = 0;
}

CLScreenCapture::~CLScreenCapture()
{
	stopCapture();
}

CScreenCapture * CLScreenCapture::getInstance(int devid)
{
	if (devid < 0 || devid >= MAX_MONITOR_NUMS)
	{
		return NULL;
	}
	
	if (NULL == m_pInstance[devid])
	{
		sys_os_mutex_enter(m_pInstMutex);

		if (NULL == m_pInstance[devid])
		{
			m_pInstance[devid] = (CScreenCapture *) new CLScreenCapture;
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

int CLScreenCapture::getDeviceNums()
{
	struct fb_var_screeninfo varinfo;
    struct fb_fix_screeninfo fixinfo;
    char device_file[12];
    int i, fd, cnt = 0;

    for (i = 0; i <= 31; i++) 
    {
        snprintf(device_file, sizeof(device_file), "/dev/fb%d", i);

        if ((fd = open(device_file, O_RDWR)) < 0) 
        {
            continue;
        }
        
        if (ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) == -1)
        {
            goto fail_device;
        }
        
        if (ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo) == -1)
        {
            goto fail_device;
		}
		
        cnt++;
        
        close(fd);
        continue;

fail_device:
        
        if (fd >= 0)
        {
			close(fd);
		}	
    }
    
    return cnt;
}

BOOL CLScreenCapture::initCapture(int codec, int width, int height, int framerate, int bitrate)
{
	CLock lock(m_pMutex);
	
	if (m_bInited)
	{
		return TRUE;
	}

	int i, fd = -1, cnt = 0;
	char device_file[12];
	struct fb_var_screeninfo varinfo; ///< framebuffer variable info
	struct fb_fix_screeninfo fixinfo; ///< framebuffer fixed info
	
	for (i = 0; i <= 31; i++) 
    {
        snprintf(device_file, sizeof(device_file), "/dev/fb%d", i);

        if ((fd = open(device_file, O_RDWR)) < 0) 
        {
        	fd = -1;
            continue;
        }
        
        if (ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) == -1)
        {
            goto fail_device;
        }
        
        if (ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo) == -1)
        {
            goto fail_device;
		}
		
		if (cnt == m_nDevIndex)
		{
			break;
		}
		
        cnt++;
        
        close(fd);
        fd = -1;        
        continue;

fail_device:
        
        if (fd >= 0)
        {
			close(fd);
			fd = -1;
		}
    }
    
	if (-1 == fd)
	{
		log_print(LOG_ERR, "%s, open failed\r\n", __FUNCTION__);
		return FALSE;
	}

    enum AVPixelFormat pix_fmt = getPixfmt(&varinfo);
    if (pix_fmt == AV_PIX_FMT_NONE) 
    {
    	close(fd);
    	log_print(LOG_ERR, "%s, getPixfmt failed\r\n", __FUNCTION__);
        return FALSE;
    }

    m_data = (uint8 *) mmap(NULL, fixinfo.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if (m_data == MAP_FAILED) 
    {
    	m_data = NULL;
        close(fd);
    	log_print(LOG_ERR, "%s, mmap failed\r\n", __FUNCTION__);
        return FALSE;
    }

	m_nFramerate = framerate;
	m_nBitrate = bitrate;
	
    m_nWidth = varinfo.xres;
	m_nHeight = varinfo.yres;

	m_smem_len = fixinfo.smem_len;

	m_bytes_per_pixel = (varinfo.bits_per_pixel + 7) >> 3;
    m_linesize  = m_nWidth * m_bytes_per_pixel;
    m_framesize	= m_linesize * m_nHeight;
    m_line_length = fixinfo.line_length;
	
	VideoEncoderParam params;
	memset(&params, 0, sizeof(params));
	
	params.SrcWidth = m_nWidth;
	params.SrcHeight = m_nHeight;
	params.SrcPixFmt = pix_fmt;
	params.DstCodec = codec;
	params.DstWidth = width ? width : m_nWidth;
	params.DstHeight = height ? height : m_nHeight;
	params.DstFramerate = framerate;
	params.DstBitrate = bitrate;
		
	if (m_encoder.init(&params) == FALSE)
	{
		return FALSE;
	}

	m_fd = fd;
	
   	m_bInited = TRUE;
   	
	return TRUE;
}

BOOL CLScreenCapture::startCapture()
{
	CLock lock(m_pMutex);
	
	if (m_hCapture)
	{
		return TRUE;
	}

	m_bCapture = TRUE;
	m_hCapture = sys_os_create_thread((void *)screenCaptureThread, this);

	return m_hCapture ? TRUE : FALSE;
}

void CLScreenCapture::stopCapture()
{
	CLock lock(m_pMutex);
	
	m_bCapture = FALSE;
	
	while (m_hCapture)
	{
		usleep(10*1000);
	}

	if (m_data)
	{
		munmap(m_data, m_smem_len);
		m_data = NULL;
	}

	if (m_fd > 0)
	{
    	close(m_fd);
    	m_fd = 0;
    }

	m_bInited = FALSE;
}

BOOL CLScreenCapture::capture()
{
	int i;
	uint8 *pin, *pout;
	AVPacket pkt;
	struct fb_var_screeninfo varinfo; ///< framebuffer variable info
	
	/* refresh fbdev->varinfo, visible data position may change at each call */
    if (ioctl(m_fd, FBIOGET_VSCREENINFO, &varinfo) < 0) 
    {
    	log_print(LOG_ERR, "%s, ioctl(FBIOGET_VSCREENINFO) failed\r\n", __FUNCTION__);
        return FALSE;
    }

	if (av_new_packet(&pkt, m_framesize) < 0)
	{
		log_print(LOG_ERR, "%s, av_new_packet failed\r\n", __FUNCTION__);
        return FALSE;
    }
    
    /* compute visible data offset */
    pin = m_data + m_bytes_per_pixel * varinfo.xoffset + varinfo.yoffset * m_line_length;
	pout = pkt.data;
	
    for (i = 0; i < m_nHeight; i++) 
    {
        memcpy(pout, pin, m_linesize);
        pin  += m_line_length;
        pout += m_linesize;
    }

    BOOL ret = m_encoder.encode(pkt.data, m_framesize);

    av_packet_unref(&pkt);
    
	return ret;
}

void CLScreenCapture::captureThread()
{	
	uint32 curtime;
	uint32 lasttime = sys_os_get_ms();
	double timestep = 1000.0 / m_nFramerate;
	double timeout;
		
	while (m_bCapture)
	{
		if (capture())
		{
			curtime = sys_os_get_ms();
			timeout = timestep - (curtime - lasttime);
			if (timeout > 10)
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




