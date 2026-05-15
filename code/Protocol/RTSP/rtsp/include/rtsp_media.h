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

#ifndef _RTSP_MEDIA_H
#define _RTSP_MEDIA_H

#include "sys_inc.h"
#include "hqueue.h"

#ifdef PROXY
#include "rtsp_proxy.h"
#endif

#ifdef PUSHER
#include "rtsp_pusher.h"
#endif

#ifdef RTSP_FILE
#include "file_demux.h"
#endif

#ifdef RTSP_DEVICE
#if __WINDOWS_OS__
#include "audio_capture_win.h"
#include "video_capture_win.h"
#include "screen_capture_win.h"
#elif __LINUX_OS__
#include "audio_capture_linux.h"
#include "video_capture_linux.h"
#include "screen_capture_linux.h"
#endif
#endif

#ifdef RTSP_LIVE
#include "live_video.h"
#include "live_audio.h"
#endif


typedef struct
{
    uint8 *     buff;
    uint8 *     data;
    int         size;
    int			nbsamples;
    int         waitnext;
} UA_PACKET;

typedef struct
{
	uint8* buff;
	uint8* data;
	int         size;
	int			type;
	int         waitnext;
	uint8* srcBuff;
	uint8* srcData;
	int         srcSize;
} UDM_PACKET;

typedef struct
{
    uint32	        has_audio	: 1;    // has audio ?
	uint32	        has_video	: 1;    // has video ?
	uint32	        metadata    : 1;    // has metadata ?
	uint32	        screen      : 1;    // is screen live
	uint32	        media_file  : 1;    // is media file
	uint32	        is_proxy    : 1;    // is proxy
	uint32	        is_pusher   : 1;    // is pusher
	uint32          is_live     : 1;    // is live stream
	uint32          rtsp_pusher : 1;    // is rtsp pusher session
	uint32	        reserved	: 23;

    char            filename[256];      // file name
    uint8           v_index;            // video stream index
    uint8           a_index;            // audio stream index
    int64           duration;           // media duration, unit is millisecond
    int64           curpos;             // media current position, unit is millisecond

	int             v_codec;            // video codec
	int             v_framerate;        // video frame rate
	int             v_bitrate;          // video bitrate
	int             v_width;            // video width
	int             v_height;           // video height

	int				a_codec;            // audio codec
	int             a_samplerate;       // audio sample rate
	int             a_channels;         // audio channel
	int             a_bitrate;          // audio bitrate

	HQUEUE *        v_queue;            // video queue
	HQUEUE *        a_queue;            // audio queue
#ifdef RTSP_METADATA
	HQUEUE *        md_queue;            // metadata queue
#endif

	pthread_t       v_thread;           // video thread
	pthread_t       a_thread;           // audio thread

#ifdef RTSP_METADATA
    pthread_t       m_thread;           // metadata thread
#endif

#ifdef PROXY
    CRtspProxy *    proxy;              // proxy object
#endif

#ifdef PUSHER
    CRtspPusher *   pusher;             // pusher object
#endif

#ifdef RTSP_FILE
	CFileDemux *    file_demuxer;       // file demuxer object
#endif

#ifdef RTSP_LIVE
    CLiveVideo *    live_video;         // live video object
    CLiveAudio *    live_audio;         // live audio object
#endif

#ifdef RTSP_DEVICE
    CVideoCapture * video_capture;      // video capture
    CAudioCapture * audio_capture;      // audio capture
	CScreenCapture* screen_capture;     // screen capture
#endif
} UA_MEDIA_INFO;

#ifdef __cplusplus
extern "C" {
#endif

BOOL    rtsp_media_init(void * p_rua);
void    rtsp_terminate_queue(void * rua);
void    rtsp_media_send_thread(void * rua);

char *  rtsp_media_get_video_sdp_line(void * rua);
char *  rtsp_media_get_audio_sdp_line(void * rua);

#ifdef __cplusplus
}
#endif

#endif


