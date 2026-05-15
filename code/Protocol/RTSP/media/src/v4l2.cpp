
#include "sys_inc.h"
#include "v4l2.h"
#include <linux/videodev2.h>


int v4l2_open_device(const char * filename)
{    
    struct v4l2_capability cap;
    int fd;
    int flags = O_RDWR;

    fd = open(filename, flags, 0);
    if (fd < 0) 
    {
        log_print(LOG_ERR, "%s, open device(%s) failed. %s\r\n", __FUNCTION__, filename, strerror(errno));
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) 
    {
        log_print(LOG_ERR, "%s, ioctl(VIDIOC_QUERYCAP) device failed. %s\r\n", __FUNCTION__, strerror(errno));
        goto fail;
    }

    log_print(H_LOG_INFO, "%s, fd:%d capabilities:%x\r\n", __FUNCTION__, fd, cap.capabilities);

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) 
    {
        log_print(LOG_ERR, "%s, Not a video capture device.\r\n", __FUNCTION__);
        goto fail;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) 
    {
        log_print(LOG_ERR, "%s, The device does not support the streaming I/O method.\r\n", __FUNCTION__);
        goto fail;
    }

    return fd;

fail:
    close(fd);
    return -1;
}


int v4l2_init_device(int fd, int *width, int *height, uint32 pixelformat)
{
    int res = 0;
    struct v4l2_format fmt;

    memset( &fmt, 0x00, sizeof( v4l2_format ) );
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = *width;
    fmt.fmt.pix.height = *height;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    /* Some drivers will fail and return EINVAL when the pixelformat
       is not supported (even if type field is valid and supported) */
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
    	log_print(LOG_ERR, "ioctl(VIDIOC_S_FMT) failed\r\n");
    	return -1;
	}
	
    if ((*width != static_cast<int>(fmt.fmt.pix.width)) || (*height != static_cast<int>(fmt.fmt.pix.height)) )
    {
    	log_print(H_LOG_INFO, "The V4L2 driver changed the video from %dx%d to %dx%d\n", 
    		*width, *height, fmt.fmt.pix.width, fmt.fmt.pix.height);
    		
        *width = fmt.fmt.pix.width;
        *height = fmt.fmt.pix.height;
    }

    if (pixelformat != fmt.fmt.pix.pixelformat) 
    {
    	log_print(LOG_DBG, "The V4L2 driver changed the pixel format from 0x%08X to 0x%08X\n",
        	pixelformat, fmt.fmt.pix.pixelformat);
        	
        res = -1;
    }

    if (fmt.fmt.pix.field == V4L2_FIELD_INTERLACED) 
    {
    	log_print(LOG_DBG, "The V4L2 driver is using the interlaced mode\n");
    }

    return res;
}




