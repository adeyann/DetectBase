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
#include "rtsp_media.h"
#include "rtsp_rsua.h"
#include "rtp_tx.h"
#include "hqueue.h"
#include "media_format.h"
#include "rtsp_cfg.h"
#include "rtsp_srv.h"
#include "rtsp_util.h"
#include "media_util.h"
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

#include <iostream>

static void rtsp_copy_from_url(char* dest, char const* src, uint32 len)
{
	// Normally, we just copy from the source to the destination.  However, if the source contains
	// %-encoded characters, then we decode them while doing the copy:
	while (len > 0)
	{
		int nBefore = 0;
		int nAfter = 0;

		if (*src == '%' && len >= 3 && sscanf(src+1, "%n%2hhx%n", &nBefore, dest, &nAfter) == 1)
		{
			uint32 codeSize = nAfter - nBefore; // should be 1 or 2

			++dest;
			src += (1 + codeSize);
			len -= (1 + codeSize);
		}
		else
		{
			*dest++ = *src++;
			--len;
		}
	}

	*dest = '\0';
}

BOOL rtsp_parse_url(char const* url, char*& username, char*& password, char*& address, int& portNum, char const** urlSuffix)
{
	do
	{
		// Parse the URL as "rtsp://[<username>[:<password>]@]<server-address-or-name>[:<port>][/<stream-name>]"
		char const* prefix = "rtsp://";
		uint32 const prefixLength = 7;
		if (strnicmp(url, prefix, prefixLength) != 0)
		{
			log_print(LOG_ERR, "%s, URL is not of the form \"%s\"\n", __FUNCTION__, prefix);
			break;
		}

		uint32 const parseBufferSize = 100;
		char parseBuffer[parseBufferSize];
		char const* from = &url[prefixLength];

		// Check whether "<username>[:<password>]@" occurs next.
		// We do this by checking whether '@' appears before the end of the URL, or before the first '/'.
		username = password = address = NULL; // default return values
		char const* colonPasswordStart = NULL;
		char const* p;

		for (p = from; *p != '\0' && *p != '/'; ++p)
		{
			if (*p == ':' && colonPasswordStart == NULL)
			{
				colonPasswordStart = p;
			}
			else if (*p == '@')
			{
				// We found <username> (and perhaps <password>).  Copy them into newly-allocated result strings:
				if (colonPasswordStart == NULL) colonPasswordStart = p;

				char const* usernameStart = from;
				uint32 usernameLen = colonPasswordStart - usernameStart;
				username = new char[usernameLen + 1] ; // allow for the trailing '\0'
				rtsp_copy_from_url(username, usernameStart, usernameLen);

				char const* passwordStart = colonPasswordStart;
				if (passwordStart < p) ++passwordStart; // skip over the ':'
				uint32 passwordLen = p - passwordStart;
				password = new char[passwordLen + 1]; // allow for the trailing '\0'
				rtsp_copy_from_url(password, passwordStart, passwordLen);

				from = p + 1; // skip over the '@'
				break;
			}
		}

		// Next, parse <server-address-or-name>
		char* to = &parseBuffer[0];
		uint32 i;

		for (i = 0; i < parseBufferSize; ++i)
		{
			if (*from == '\0' || *from == ':' || *from == '/')
			{
				// We've completed parsing the address
				*to = '\0';
				break;
			}
			*to++ = *from++;
		}

		if (i == parseBufferSize)
		{
			log_print(LOG_ERR, "%s, URL is too long\r\n", __FUNCTION__);
			break;
		}

		address = new char[strlen(parseBuffer)+1] ;
		strcpy(address, parseBuffer);

		portNum = 554; // default value
		char nextChar = *from;
		if (nextChar == ':')
		{
			int portNumInt;
			if (sscanf(++from, "%d", &portNumInt) != 1)
			{
				log_print(LOG_ERR, "%s, No port number follows ':'\r\n", __FUNCTION__);
				break;
			}

			if (portNumInt < 1 || portNumInt > 65535)
			{
				log_print(LOG_ERR, "%s, Bad port number", __FUNCTION__);
				break;
			}

			portNum = portNumInt;

			while (*from >= '0' && *from <= '9')
			{
				++from; // skip over port number
			}
		}

		// The remainder of the URL is the suffix:
		if (urlSuffix != NULL)
		{
			*urlSuffix = from;
		}

		return TRUE;
	} while (0);

  	return FALSE;
}

const char * rtsp_get_url_suffix(const char * url)
{
	char* username = NULL;
	char* password = NULL;
	char* address = NULL;
	const char *urlSuffix = NULL;
	int   urlPortNum = 554;

	if (!rtsp_parse_url(url, username, password, address, urlPortNum, &urlSuffix))
	{
		log_print(LOG_ERR, "%s, rtsp_parse_url failed. %s\r\n", __FUNCTION__, url);
		return NULL;
	}

	if (username)
	{
		delete[] username;
	}

	if (password)
	{
		delete[] password;
	}

	delete[] address;

	return urlSuffix;
}

char * rtsp_media_get_video_sdp_line(void * rua)
{
	RSUA * p_rua = (RSUA *)rua;

#ifdef PROXY
    if (p_rua->media_info.is_proxy)
    {
        if (p_rua->media_info.proxy)
        {
            return p_rua->media_info.proxy->getVideoAuxSDPLine(p_rua->self_video_cap[0]);
        }
    }
#endif

#ifdef PUSHER
    if (p_rua->media_info.is_pusher)
    {
        if (p_rua->media_info.pusher)
        {
            return p_rua->media_info.pusher->getVideoAuxSDPLine(p_rua->self_video_cap[0]);
        }
    }
#endif

#ifdef RTSP_FILE
	if (p_rua->media_info.media_file)
	{
		if (p_rua->media_info.file_demuxer)
		{
			return p_rua->media_info.file_demuxer->getVideoAuxSDPLine(p_rua->self_video_cap[0]);
		}
	}
#endif

#ifdef RTSP_DEVICE
	if (p_rua->media_info.screen)
	{
		if (p_rua->media_info.screen_capture)
		{
			return p_rua->media_info.screen_capture->getAuxSDPLine(p_rua->self_video_cap[0]);
		}
	}
	else if (p_rua->media_info.video_capture)
	{
		return p_rua->media_info.video_capture->getAuxSDPLine(p_rua->self_video_cap[0]);
	}
#endif

#ifdef RTSP_LIVE
	if (p_rua->media_info.is_live)
	{
		if (p_rua->media_info.live_video)
		{
			return p_rua->media_info.live_video->getAuxSDPLine(p_rua->self_video_cap[0]);
		}
	}
#endif

	return NULL;
}


char * rtsp_media_get_audio_sdp_line(void * rua)
{
	RSUA * p_rua = (RSUA *)rua;

#ifdef PROXY
    if (p_rua->media_info.is_proxy)
    {
        if (p_rua->media_info.proxy)
        {
            return p_rua->media_info.proxy->getAudioAuxSDPLine(p_rua->self_audio_cap[0]);
        }
    }
#endif

#ifdef PUSHER
    if (p_rua->media_info.is_pusher)
    {
        if (p_rua->media_info.pusher)
        {
            return p_rua->media_info.pusher->getAudioAuxSDPLine(p_rua->self_audio_cap[0]);
        }
    }
#endif

#ifdef RTSP_FILE
	if (p_rua->media_info.media_file)
	{
		if (p_rua->media_info.file_demuxer)
		{
			return p_rua->media_info.file_demuxer->getAudioAuxSDPLine(p_rua->self_audio_cap[0]);
		}
	}
#endif

#ifdef RTSP_DEVICE
	if (p_rua->media_info.audio_capture)
	{
		return p_rua->media_info.audio_capture->getAuxSDPLine(p_rua->self_audio_cap[0]);
	}
#endif

#ifdef RTSP_LIVE
	if (p_rua->media_info.is_live)
	{
		if (p_rua->media_info.live_audio)
		{
			return p_rua->media_info.live_audio->getAuxSDPLine(p_rua->self_audio_cap[0]);
		}
	}
#endif

	return NULL;
}

void rtsp_media_fix_audio_param(RSUA * p_rua)
{
	if (AUDIO_CODEC_G726 == p_rua->media_info.a_codec)
	{
		p_rua->media_info.a_channels = 1; // G726 only support mono
		p_rua->media_info.a_samplerate = 8000;
		return;
	}

	if (AUDIO_CODEC_G722 == p_rua->media_info.a_codec)
	{
		p_rua->media_info.a_channels = 1; // G722 only support mono
		p_rua->media_info.a_samplerate = 16000;
		return;
	}

	if (AUDIO_CODEC_OPUS == p_rua->media_info.a_codec)
	{
		p_rua->media_info.a_channels = 2;
		p_rua->media_info.a_samplerate = 48000;
		return;
	}

	const int sample_rates[] =
	{
		8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
	};

	int i;
	int sample_rate_num = sizeof(sample_rates) / sizeof(int);

	for (i = 0; i < sample_rate_num; i++)
	{
		if (p_rua->media_info.a_samplerate <= sample_rates[i])
		{
			p_rua->media_info.a_samplerate = sample_rates[i];
			break;
		}
	}

	if (i == sample_rate_num)
	{
		p_rua->media_info.a_samplerate = 48000;
	}
}

BOOL rtsp_parse_url_transfer_parameters(RSUA * p_rua)
{
	char value[32] = {'\0'};
	char * p = strchr(p_rua->media_info.filename, '&');
	if (NULL == p)
	{
		return FALSE;
	}

	p++; // skip '&' char

	if (GetNameValuePair(p, strlen(p), "t", value, sizeof(value)))
	{
		if (strcasecmp(value, "unicast") == 0)
		{
			p_rua->rtp_unicast = 1;
		}
		else if (strcasecmp(value, "multicase") == 0)
		{
			p_rua->rtp_unicast = 0;
		}
	}

	if (GetNameValuePair(p, strlen(p), "p", value, sizeof(value)))
	{
		if (strcasecmp(value, "udp") == 0)
		{
			p_rua->rtp_tcp = 0;
		}
		else if (strcasecmp(value, "tcp") == 0)
		{
			p_rua->rtp_tcp = 1;
		}
		else if (strcasecmp(value, "rtsp") == 0)
		{
			p_rua->rtp_tcp = 1;
		}
		else if (strcasecmp(value, "http") == 0)
		{

		}
	}

	return TRUE;
}

BOOL rtsp_parse_url_video_parameters(RSUA * p_rua)
{
	char value[32] = {'\0'};
	char * p = strchr(p_rua->media_info.filename, '&');
	if (NULL == p)
	{
		return FALSE;
	}

	p++; // skip '&' char

	if (GetNameValuePair(p, strlen(p), "ve", value, sizeof(value)))
	{
		if (strcasecmp(value, "JPEG") == 0)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_JPEG;
		}
		else if (strcasecmp(value, "H264") == 0)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_H264;
		}
		else if (strcasecmp(value, "H265") == 0)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_H265;
		}
		else if (strcasecmp(value, "MPV4-ES") == 0)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_MP4;
		}
	}

	if (GetNameValuePair(p, strlen(p), "w", value, sizeof(value)))
	{
		p_rua->media_info.v_width = atoi(value);
	}

	if (GetNameValuePair(p, strlen(p), "h", value, sizeof(value)))
	{
		p_rua->media_info.v_height = atoi(value);
	}

	return TRUE;
}

BOOL rtsp_parse_url_audio_parameters(RSUA * p_rua)
{
	char value[32] = {'\0'};
	char * p = strchr(p_rua->media_info.filename, '&');
	if (NULL == p)
	{
		return FALSE;
	}

	p++; // skip '&' char

	if (GetNameValuePair(p, strlen(p), "ae", value, sizeof(value)))
	{
		if (strcasecmp(value, "PCMU") == 0)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_G711U;
		}
		else if (strcasecmp(value, "G726") == 0)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_G726;
		}
		else if (strcasecmp(value, "MP4A-LATM") == 0)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_AAC;
		}
	}

	if (GetNameValuePair(p, strlen(p), "sr", value, sizeof(value)))
	{
		p_rua->media_info.a_samplerate = atoi(value);
	}

	return TRUE;
}


#ifdef PROXY
BOOL rtsp_media_proxy_init(RSUA * p_rua, RTSP_PROXY * p_proxy)
{
    CRtspProxy * proxy = p_proxy->proxy;

    if (NULL == proxy || proxy->m_inited == 0)
    {
        log_print(H_LOG_WARN, "%s, proxy uninit, suffix = %s, url = %s\r\n",
            __FUNCTION__, p_proxy->cfg.suffix, p_proxy->cfg.url);
        return FALSE;
    }

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int vrecodec = proxy->getVideoRecodec();
    int arecodec = proxy->getAudioRecodec();
#endif

    p_rua->media_info.is_proxy = 1;
    p_rua->media_info.proxy = p_proxy->proxy;

    if (proxy->m_has_video)
    {
        p_rua->media_info.has_video = p_proxy->proxy->m_has_video;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
        if (1 == vrecodec)
        {
            p_rua->media_info.v_codec = p_proxy->cfg.output.v_info.codec;
            p_rua->media_info.v_width = p_proxy->cfg.output.v_info.width;
            p_rua->media_info.v_height = p_proxy->cfg.output.v_info.height;
			p_rua->media_info.v_framerate = p_proxy->cfg.output.v_info.framerate;
            p_rua->media_info.v_bitrate = p_proxy->cfg.output.v_info.bitrate;
        }
        else
#endif
        {
            p_rua->media_info.v_codec = proxy->m_v_codec;
            p_rua->media_info.v_width = proxy->m_v_width;
            p_rua->media_info.v_height = proxy->m_v_height;
        }
	}

    if (proxy->m_has_audio)
    {
        p_rua->media_info.has_audio = p_proxy->proxy->m_has_audio;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
        if (1 == arecodec)
        {
            p_rua->media_info.a_codec = p_proxy->cfg.output.a_info.codec;
            p_rua->media_info.a_samplerate = p_proxy->cfg.output.a_info.samplerate;
            p_rua->media_info.a_channels = p_proxy->cfg.output.a_info.channels;
            p_rua->media_info.a_bitrate = p_proxy->cfg.output.a_info.bitrate;
        }
        else
#endif
        {
            p_rua->media_info.a_codec = p_proxy->proxy->m_a_codec;
            p_rua->media_info.a_samplerate = p_proxy->proxy->m_a_samplerate;
            p_rua->media_info.a_channels = p_proxy->proxy->m_a_channels;
        }
	}

    return TRUE;
}
#endif // end of PROXY

#ifdef PUSHER
BOOL rtsp_media_pusher_init(RSUA * p_rua, RTSP_PUSHER * p_pusher)
{
    CRtspPusher * pusher = p_pusher->pusher;

    if (NULL == pusher || pusher->isInited() == FALSE)
    {
        log_print(H_LOG_WARN, "%s, pusher uninit, suffix = %s\r\n", __FUNCTION__, p_pusher->cfg.suffix);
        return FALSE;
    }

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int vrecodec = pusher->getVideoRecodec();
    int arecodec = pusher->getAudioRecodec();
#endif

    p_rua->media_info.is_pusher = 1;
    p_rua->media_info.pusher = pusher;

    if (p_pusher->cfg.has_video)
    {
        p_rua->media_info.has_video = p_pusher->cfg.has_video;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
        if (1 == vrecodec)
        {
            p_rua->media_info.v_codec = p_pusher->cfg.output.v_info.codec;
            p_rua->media_info.v_width = p_pusher->cfg.output.v_info.width;
            p_rua->media_info.v_height = p_pusher->cfg.output.v_info.height;
            p_rua->media_info.v_framerate = p_pusher->cfg.output.v_info.framerate;
            p_rua->media_info.v_bitrate = p_pusher->cfg.output.v_info.bitrate;
        }
        else
#endif
        {
            p_rua->media_info.v_codec = p_pusher->cfg.v_info.codec;
            p_rua->media_info.v_width = p_pusher->cfg.v_info.width;
            p_rua->media_info.v_height = p_pusher->cfg.v_info.height;
            p_rua->media_info.v_framerate = p_pusher->cfg.v_info.framerate;
            p_rua->media_info.v_bitrate = p_pusher->cfg.v_info.bitrate;
        }
    }

    if (p_pusher->cfg.has_audio)
    {
        p_rua->media_info.has_audio = p_pusher->cfg.has_audio;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
        if (1 == arecodec)
        {
            p_rua->media_info.a_codec = p_pusher->cfg.output.a_info.codec;
            p_rua->media_info.a_samplerate = p_pusher->cfg.output.a_info.samplerate;
            p_rua->media_info.a_channels = p_pusher->cfg.output.a_info.channels;
            p_rua->media_info.a_bitrate = p_pusher->cfg.output.a_info.bitrate;
        }
        else
#endif
        {
            p_rua->media_info.a_codec = p_pusher->cfg.a_info.codec;
            p_rua->media_info.a_samplerate = p_pusher->cfg.a_info.samplerate;
            p_rua->media_info.a_channels = p_pusher->cfg.a_info.channels;
            p_rua->media_info.a_bitrate = p_pusher->cfg.a_info.bitrate;
        }
    }

    return TRUE;
}
#endif // end of PUSHER


#ifdef RTSP_DEVICE
BOOL rtsp_parse_device_suffix(RSUA * p_rua, const char * suffix)
{
	int i = 0;
	char buff[100];

	char filename[256] = {'\0'};
	const char * p = strchr(suffix, '&');
	if (p)
	{
		strncpy(filename, suffix, p-suffix);
	}
	else
	{
		strcpy(filename, suffix);
	}

	while (filename[i] != '\0')
	{
		if (filename[i] == '+')
		{
			break;
		}
		else
		{
			buff[i] = filename[i];
		}

		i++;
	}

	buff[i] = '\0';

	int screen = 0;
	int vdevice = 0;
	int adevice = 0;
	int vindex = 0;
	int aindex = 0;
	int len = strlen("videodevice");

	if (strnicmp(buff, "videodevice", strlen("videodevice")) == 0)
	{
		vdevice = 1;

		len = strlen("videodevice");
		if (buff[len] != '\0')
		{
			vindex = atoi(buff+len);
		}
	}
	else if (strnicmp(buff, "audiodevice", strlen("audiodevice")) == 0)
	{
		adevice = 1;

		len = strlen("audiodevice");
		if (buff[len] != '\0')
		{
			aindex = atoi(buff+len);
		}
	}
	else if (strnicmp(buff, "screenlive", strlen("screenlive")) == 0)
	{
		screen = 1;

		len = strlen("screenlive");
		if (buff[len] != '\0')
		{
			vindex = atoi(buff+len);
		}
	}
	else
	{
		return FALSE;
	}

	if (filename[i] == '+')
	{
		strcpy(buff, filename+i+1);

		if (strnicmp(buff, "videodevice", strlen("videodevice")) == 0)
		{
			vdevice = 1;

			len = strlen("videodevice");
			if (buff[len] != '\0')
			{
				vindex = atoi(buff+len);
			}
		}
		else if (strnicmp(buff, "audiodevice", strlen("audiodevice")) == 0)
		{
			adevice = 1;

			len = strlen("audiodevice");
			if (buff[len] != '\0')
			{
				aindex = atoi(buff+len);
			}
		}
		else if (strnicmp(buff, "screenlive", strlen("screenlive")) == 0)
		{
			screen = 1;

			len = strlen("screenlive");
			if (buff[len] != '\0')
			{
				vindex = atoi(buff+len);
			}
		}
		else
		{
			return FALSE;
		}
	}

	if (vdevice)
	{
		p_rua->media_info.has_video = 1;
		p_rua->media_info.v_index = vindex;
	}

	if (adevice)
	{
		p_rua->media_info.has_audio = 1;
		p_rua->media_info.a_index = aindex;
	}

	if (screen)
	{
		p_rua->media_info.has_video = 1;
		p_rua->media_info.screen = 1;
		p_rua->media_info.v_index = vindex;
	}

	return TRUE;
}

BOOL rtsp_media_device_init(RSUA * p_rua)
{
	char filename[256] = {'\0'};
	char * p = strchr(p_rua->media_info.filename, '&');
	if (p)
	{
		strncpy(filename, p_rua->media_info.filename, p-p_rua->media_info.filename);
	}
	else
	{
		strcpy(filename, p_rua->media_info.filename);
	}

	if (p_rua->media_info.has_video)
	{
		rtsp_parse_url_transfer_parameters(p_rua);

		// screen capture
		if (p_rua->media_info.screen)
		{
#if __WINDOWS_OS__
			if (p_rua->media_info.v_index < 0 ||
				p_rua->media_info.v_index >= CWScreenCapture::getDeviceNums())
			{
				log_print(LOG_ERR, "%s, v_index=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.v_index, CWScreenCapture::getDeviceNums());
				return FALSE;
			}
#elif __LINUX_OS__
			if (p_rua->media_info.v_index < 0 ||
				p_rua->media_info.v_index >= CLScreenCapture::getDeviceNums())
			{
				log_print(LOG_ERR, "%s, v_index=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.v_index, CLScreenCapture::getDeviceNums());
				return FALSE;
			}
#endif

			// get video output configuration
			rtsp_cfg_get_video_info(filename,
									&p_rua->media_info.v_codec,
									&p_rua->media_info.v_width,
									&p_rua->media_info.v_height,
									&p_rua->media_info.v_framerate,
									&p_rua->media_info.v_bitrate);

			if (VIDEO_CODEC_NONE == p_rua->media_info.v_codec)
			{
				p_rua->media_info.v_codec = VIDEO_CODEC_H264;
			}

			if (0 == p_rua->media_info.v_framerate)
			{
				p_rua->media_info.v_framerate = 15;
			}

			rtsp_parse_url_video_parameters(p_rua);

			// get screen capture instance
#if __WINDOWS_OS__
			p_rua->media_info.screen_capture = CWScreenCapture::getInstance(p_rua->media_info.v_index);
#elif __LINUX_OS__
			p_rua->media_info.screen_capture = CLScreenCapture::getInstance(p_rua->media_info.v_index);
#endif
			if (p_rua->media_info.screen_capture->initCapture(p_rua->media_info.v_codec,
															  p_rua->media_info.v_width,
															  p_rua->media_info.v_height,
															  p_rua->media_info.v_framerate,
															  p_rua->media_info.v_bitrate) == FALSE)
			{
				log_print(LOG_ERR, "%s, init screen capture failed\r\n", __FUNCTION__);
				p_rua->media_info.screen_capture->freeInstance(p_rua->media_info.v_index);
				p_rua->media_info.screen_capture = NULL;
				return FALSE;
			}

			// get video size
			if (p_rua->media_info.v_width == 0 || p_rua->media_info.v_height == 0)
			{
				p_rua->media_info.v_width = p_rua->media_info.screen_capture->getWidth();
				p_rua->media_info.v_height = p_rua->media_info.screen_capture->getHeight();
			}
		}
		else // video capture from camera
		{
#if __WINDOWS_OS__
			if (p_rua->media_info.v_index < 0 ||
				p_rua->media_info.v_index >= CWVideoCapture::getDeviceNums())
			{
				log_print(LOG_ERR, "%s, v_index=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.v_index, CWVideoCapture::getDeviceNums());
				return FALSE;
			}
#elif __LINUX_OS__
			if (p_rua->media_info.v_index < 0 ||
				p_rua->media_info.v_index >= CLVideoCapture::getDeviceNums())
			{
				log_print(LOG_ERR, "%s, v_index=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.v_index, CLVideoCapture::getDeviceNums());
				return FALSE;
			}
#endif

			// get video output configuration
			rtsp_cfg_get_video_info(filename,
									&p_rua->media_info.v_codec,
									&p_rua->media_info.v_width,
									&p_rua->media_info.v_height,
									&p_rua->media_info.v_framerate,
									&p_rua->media_info.v_bitrate);

			if (VIDEO_CODEC_NONE == p_rua->media_info.v_codec)
			{
				p_rua->media_info.v_codec = VIDEO_CODEC_H264;
			}

			if (0 == p_rua->media_info.v_framerate)
			{
				p_rua->media_info.v_framerate = 25;
			}

			rtsp_parse_url_video_parameters(p_rua);

			// get video capture instance
#if __WINDOWS_OS__
			p_rua->media_info.video_capture = CWVideoCapture::getInstance(p_rua->media_info.v_index);
#elif __LINUX_OS__
			p_rua->media_info.video_capture = CLVideoCapture::getInstance(p_rua->media_info.v_index);
#endif
			if (p_rua->media_info.video_capture->initCapture(p_rua->media_info.v_codec,
															 p_rua->media_info.v_width,
															 p_rua->media_info.v_height,
															 p_rua->media_info.v_framerate,
															 p_rua->media_info.v_bitrate) == FALSE)
			{
				log_print(LOG_ERR, "%s, init video capture failed\r\n", __FUNCTION__);
				p_rua->media_info.video_capture->freeInstance(p_rua->media_info.v_index);
				p_rua->media_info.video_capture = NULL;
				return FALSE;
			}

			// get video sizes
			if (p_rua->media_info.v_width == 0 || p_rua->media_info.v_height == 0)
			{
				p_rua->media_info.v_width = p_rua->media_info.video_capture->getWidth();
				p_rua->media_info.v_height = p_rua->media_info.video_capture->getHeight();
			}
		}
	}

	if (p_rua->media_info.has_audio)
	{
#if __WINDOWS_OS__
		if (p_rua->media_info.a_index < 0 ||
			p_rua->media_info.a_index >= CWAudioCapture::getDeviceNums())
		{
			log_print(LOG_ERR, "%s, aindex=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.a_index, CWAudioCapture::getDeviceNums());
			return FALSE;
		}
#elif __LINUX_OS__
		if (p_rua->media_info.a_index < 0 ||
			p_rua->media_info.a_index >= CLAudioCapture::getDeviceNums())
		{
			log_print(LOG_ERR, "%s, aindex=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.a_index, CLAudioCapture::getDeviceNums());
			return FALSE;
		}
#endif

		// get audio output configuration
		rtsp_cfg_get_audio_info(filename,
								&p_rua->media_info.a_codec,
								&p_rua->media_info.a_samplerate,
								&p_rua->media_info.a_channels,
								&p_rua->media_info.a_bitrate);

		if (AUDIO_CODEC_NONE == p_rua->media_info.a_codec)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_G711U;
		}

		if (0 == p_rua->media_info.a_samplerate)
		{
			p_rua->media_info.a_samplerate = 8000;
		}

		if (0 == p_rua->media_info.a_channels)
		{
			p_rua->media_info.a_channels = 1;
		}

		rtsp_parse_url_audio_parameters(p_rua);

		rtsp_media_fix_audio_param(p_rua);

		// get audio capture instance
#if __WINDOWS_OS__
		p_rua->media_info.audio_capture = CWAudioCapture::getInstance(p_rua->media_info.a_index);
#elif __LINUX_OS__
		p_rua->media_info.audio_capture = CLAudioCapture::getInstance(p_rua->media_info.a_index);
#endif
		if (p_rua->media_info.audio_capture->initCapture(p_rua->media_info.a_codec,
		                                                 p_rua->media_info.a_samplerate,
		                                                 p_rua->media_info.a_channels,
		                                                 p_rua->media_info.a_bitrate) == FALSE)
		{
			log_print(LOG_ERR, "%s, init audio capture failed\r\n", __FUNCTION__);
			p_rua->media_info.audio_capture->freeInstance(p_rua->media_info.a_index);
			p_rua->media_info.audio_capture = NULL;
			return FALSE;
		}
	}

	return TRUE;
}
#endif // end of RTSP_DEVICE

#ifdef RTSP_FILE
BOOL rtsp_media_file_init(RSUA * p_rua)
{
	char filename[256] = {'\0'};
	char * p = strchr(p_rua->media_info.filename, '&');
	if (p)
	{
		strncpy(filename, p_rua->media_info.filename, p-p_rua->media_info.filename);
	}
	else
	{
		strcpy(filename, p_rua->media_info.filename);
	}

	p_rua->media_info.file_demuxer = new CFileDemux(filename);
	if (NULL == p_rua->media_info.file_demuxer)
	{
		return FALSE;
	}

	p_rua->media_info.duration = p_rua->media_info.file_demuxer->getDuration();

	if (p_rua->media_info.file_demuxer->hasVideo())
	{
		p_rua->media_info.has_video = 1;

		// get video output configuration
		rtsp_cfg_get_video_info(filename,
								&p_rua->media_info.v_codec,
								&p_rua->media_info.v_width,
								&p_rua->media_info.v_height,
								&p_rua->media_info.v_framerate,
								&p_rua->media_info.v_bitrate);

		if (VIDEO_CODEC_NONE == p_rua->media_info.v_codec)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_H264;
		}

		// parse the url parameters
		rtsp_parse_url_transfer_parameters(p_rua);
		rtsp_parse_url_video_parameters(p_rua);

		if (p_rua->media_info.v_width <= 0 || p_rua->media_info.v_height <= 0)
		{
			p_rua->media_info.v_width = p_rua->media_info.file_demuxer->getWidth();
			p_rua->media_info.v_height = p_rua->media_info.file_demuxer->getHeight();
		}

		if (0 == p_rua->media_info.v_framerate)
		{
			p_rua->media_info.v_framerate = p_rua->media_info.file_demuxer->getFramerate();
		}

		if (p_rua->media_info.file_demuxer->setVideoFormat(p_rua->media_info.v_codec,
														   p_rua->media_info.v_width,
														   p_rua->media_info.v_height,
														   p_rua->media_info.v_framerate,
														   p_rua->media_info.v_bitrate) == FALSE)
		{
			delete p_rua->media_info.file_demuxer;
			p_rua->media_info.file_demuxer = NULL;
			return FALSE;
		}
	}

	if (p_rua->media_info.file_demuxer->hasAudio())
	{
		p_rua->media_info.has_audio = 1;

		// get audio output configuration
		rtsp_cfg_get_audio_info(filename,
								&p_rua->media_info.a_codec,
								&p_rua->media_info.a_samplerate,
								&p_rua->media_info.a_channels,
								&p_rua->media_info.a_bitrate);

		if (AUDIO_CODEC_NONE == p_rua->media_info.a_codec)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_G711U;
		}

		if (0 == p_rua->media_info.a_samplerate)
		{
			p_rua->media_info.a_samplerate = p_rua->media_info.file_demuxer->getSamplerate();
		}

		if (0 == p_rua->media_info.a_channels)
		{
			p_rua->media_info.a_channels = p_rua->media_info.file_demuxer->getChannels();
		}

		if (p_rua->media_info.a_channels > 2)
		{
		    p_rua->media_info.a_channels = 2;
		}

		// parse the url parameters
		rtsp_parse_url_transfer_parameters(p_rua);
		rtsp_parse_url_audio_parameters(p_rua);

		rtsp_media_fix_audio_param(p_rua);

		if (p_rua->media_info.file_demuxer->setAudioFormat(p_rua->media_info.a_codec,
		                                                   p_rua->media_info.a_samplerate,
		                                                   p_rua->media_info.a_channels,
		                                                   p_rua->media_info.a_bitrate) == FALSE)
		{
			delete p_rua->media_info.file_demuxer;
			p_rua->media_info.file_demuxer = NULL;
			return FALSE;
		}
	}

	return (p_rua->media_info.has_video || p_rua->media_info.has_audio);

}
#endif // end of RTSP_FILE

#ifdef RTSP_LIVE
BOOL rtsp_parse_live_suffix(RSUA * p_rua, const char * suffix)
{
	// todo : parse the URL suffix, set the p_rua->media_info struct

	if (strnicmp(suffix, "live", strlen("live")) != 0)
	{
		return FALSE;
	}

	p_rua->media_info.is_live = 1;

	p_rua->media_info.has_video = 1;
	p_rua->media_info.v_codec = VIDEO_CODEC_H264;
	p_rua->media_info.v_framerate = 25;
	p_rua->media_info.v_index = 0;

	// todo : if have audio, uncomment the statements

	// p_rua->media_info.has_audio = 1;
	// p_rua->media_info.a_codec = AUDIO_CODEC_G711U;
	// p_rua->media_info.a_samplerate = 8000;
	// p_rua->media_info.a_channels = 1;
	// p_rua->media_info.a_index = 0;

	return TRUE;
}

BOOL rtsp_media_live_init(RSUA * p_rua)
{
	// todo : init the live stream object

	if (p_rua->media_info.has_video)
	{
		rtsp_parse_url_transfer_parameters(p_rua);

		if (p_rua->media_info.v_index < 0 ||
			p_rua->media_info.v_index >= CLiveVideo::getStreamNums())
		{
			log_print(LOG_ERR, "%s, v_index=%d, getStreamNums=%d\r\n", __FUNCTION__,
			p_rua->media_info.v_index, CLiveVideo::getStreamNums());
			return FALSE;
		}

		if (VIDEO_CODEC_NONE == p_rua->media_info.v_codec)
		{
			p_rua->media_info.v_codec = VIDEO_CODEC_H264;
		}

		if (0 == p_rua->media_info.v_framerate)
		{
			p_rua->media_info.v_framerate = 25;
		}

		// According the need, you can uncomment the statement
		// rtsp_parse_url_video_parameters(p_rua);

		// get the live video instance
		p_rua->media_info.live_video = CLiveVideo::getInstance(p_rua->media_info.v_index);
		if (p_rua->media_info.live_video->initCapture(p_rua->media_info.v_codec,
													  p_rua->media_info.v_width,
													  p_rua->media_info.v_height,
													  p_rua->media_info.v_framerate,
													  p_rua->media_info.v_bitrate) == FALSE)
		{
			log_print(LOG_ERR, "%s, init live video capture failed\r\n", __FUNCTION__);
			p_rua->media_info.live_video->freeInstance(p_rua->media_info.v_index);
			p_rua->media_info.live_video = NULL;
			return FALSE;
		}
	}

	if (p_rua->media_info.has_audio)
	{
		if (p_rua->media_info.a_index < 0 ||
			p_rua->media_info.a_index >= CLiveAudio::getDeviceNums())
		{
			log_print(LOG_ERR, "%s, aindex=%d, getDeviceNums=%d\r\n", __FUNCTION__,
				p_rua->media_info.a_index, CLiveAudio::getDeviceNums());
			return FALSE;
		}

		if (AUDIO_CODEC_NONE == p_rua->media_info.a_codec)
		{
			p_rua->media_info.a_codec = AUDIO_CODEC_G711U;
		}

		if (0 == p_rua->media_info.a_samplerate)
		{
			p_rua->media_info.a_samplerate = 8000;
		}

		if (0 == p_rua->media_info.a_channels)
		{
			p_rua->media_info.a_channels = 1;
		}

		// According the need, you can uncomment the statement
		// rtsp_parse_url_audio_parameters(p_rua);

		rtsp_media_fix_audio_param(p_rua);

		// get audio capture instance
		p_rua->media_info.live_audio = CLiveAudio::getInstance(p_rua->media_info.a_index);
		if (p_rua->media_info.live_audio->initCapture(p_rua->media_info.a_codec,
		                                              p_rua->media_info.a_samplerate,
		                                              p_rua->media_info.a_channels,
		                                              p_rua->media_info.a_bitrate) == FALSE)
		{
			log_print(LOG_ERR, "%s, init live audio capture failed\r\n", __FUNCTION__);
			p_rua->media_info.live_audio->freeInstance(p_rua->media_info.a_index);
			p_rua->media_info.live_audio = NULL;
			return FALSE;
		}
	}

	return TRUE;
}
#endif	// end of RTSP_LIVE

BOOL rtsp_media_init(void * rua)
{
	RSUA * p_rua = (RSUA *)rua;

	const char * p_suffix = rtsp_get_url_suffix(p_rua->uri);
	if (NULL == p_suffix)
	{
		return FALSE;
	}

	strcpy(p_rua->media_info.filename, p_suffix+1);

#ifdef PROXY
    RTSP_PROXY * p_proxy = rtsp_proxy_match(p_suffix+1);
    if (p_proxy)
    {
        return rtsp_media_proxy_init(p_rua, p_proxy);
    }
#endif

#ifdef PUSHER
    RTSP_PUSHER * p_pusher = rtsp_pusher_match(p_suffix+1);
    if (p_pusher)
    {
        return rtsp_media_pusher_init(p_rua, p_pusher);
    }
#endif

#ifdef RTSP_DEVICE
	if (rtsp_parse_device_suffix(p_rua, p_suffix+1))
	{
		return rtsp_media_device_init(p_rua);
	}
#endif

#ifdef RTSP_LIVE
	if (rtsp_parse_live_suffix(p_rua, p_suffix+1))
	{
		return rtsp_media_live_init(p_rua);
	}
#endif

#ifdef RTSP_FILE
	{
		p_rua->media_info.media_file = 1;

		return rtsp_media_file_init(p_rua);
	}
#endif

    return TRUE;
}

void rtsp_media_clear_queue(HQUEUE * queue)
{
	UA_PACKET packet;

	while (!hqBufIsEmpty(queue))
	{
		if (hqBufGet(queue, (char *)&packet))
		{
			if (packet.data != NULL && packet.size != 0)
			{
				free(packet.buff);
				packet.buff = NULL;
			}
		}
		else
		{
		    // should be not to here
		    log_print(LOG_ERR, "%s, hqBufGet failed\r\n", __FUNCTION__);
		    break;
		}
	}
	//printf("queue cleared\n");
}

// per Proxy
void * rtsp_media_video_thread(void * argv)
{
    int sret = -1;
	RSUA * p_rua = (RSUA *)argv;
	UA_PACKET packet;
	int framerate = p_rua->media_info.v_framerate;

	while (p_rua->rtp_tx)
	{
	    if (p_rua->rtp_pause)
	    {
	        usleep(10*1000);
	        continue;
	    }

		if (hqBufGet(p_rua->media_info.v_queue, (char *)&packet))
		{
			int timeStamp = 90000;
			// 다음 패킷이 있으면 DelayQueue에 패킷을 넣고 다시 뺀다.
			//
			if (packet.data == NULL || packet.size == 0)
			{
				//printf("rtsp_media_video_thread received empty packet");
				break;
			}
			else if (p_rua->media_info.v_codec == VIDEO_CODEC_H264)
			{
				sret = rtp_h264_video_tx(p_rua, packet.data, packet.size, rtsp_get_timestamp( timeStamp ) );
				//std::cout << p_rua->media_info.proxy->m_pConfig->suffix << " send h264 video is " << sret << std::endl;
			} else if ( p_rua->media_info.v_codec == VIDEO_CODEC_H265 )
			{
				//std::cout << "send h265 video " << p_rua->media_info.proxy->m_pConfig->suffix << std::endl;
				sret = rtp_h265_video_tx( p_rua, packet.data, packet.size, rtsp_get_timestamp( timeStamp ) );
			} else if ( p_rua->media_info.v_codec == VIDEO_CODEC_MP4 )
			{
				//std::cout << "send mp4 video " << p_rua->media_info.proxy->m_pConfig->suffix << std::endl;
				sret = rtp_video_tx( p_rua, packet.data, packet.size, rtsp_get_timestamp( timeStamp ) );
			} else if ( p_rua->media_info.v_codec == VIDEO_CODEC_JPEG )
			{
				//std::cout << "send jpeg video " << p_rua->media_info.proxy->m_pConfig->suffix << std::endl;
				sret = rtp_jpeg_video_tx( p_rua, packet.data, packet.size, rtsp_get_timestamp( timeStamp ));
			}

			free(packet.buff);

#ifdef RTSP_REPLAY
            if (p_rua->replay && !p_rua->rate_control)
            {
                usleep(10*1000);
                continue;
            }
#endif
			// waitnext는 framerate 기간동안 기다린다.
			// 이전패킷의 rtp타임과 보낸패킷의 rtptime을 비교한다.
			// waitnext가 없으면 그냥 queue에 있는걸 그냥 다 가져와 버린다.
			// rtp 패킷의 timestamp를 확인하고 프레임레이트에 따라서 적절히 패킷을 빼내는 작업을 한다.
			if (packet.waitnext)
			{
				//usleep(1000000/framerate);
				std::this_thread::sleep_for( std::chrono::milliseconds( 1000 / framerate ) );
			}
		}
		else
		{
		    // should be not to here
		    log_print(LOG_ERR, "%s, hqBufGet failed\r\n", __FUNCTION__);
		    break;
		}
	}
	//printf("rtsp_media_video_thread stopped receiving video data");

	//printf("End of video thread try and clear video queue");
	rtsp_media_clear_queue(p_rua->media_info.v_queue);
	//printf("End of video thread cleared video queue");

#ifdef RTSP_METADATA
	//printf("End of video thread try and clear meta data queue");
	rtsp_media_clear_queue(p_rua->media_info.md_queue);
	//printf("End of video thread cleared meta data queue");
#endif
	p_rua->media_info.v_thread = 0;

	UNUSED( sret );
	//printf("stopped media video thread");
	return NULL;
}

void * rtsp_media_audio_thread(void * argv)
{
    int sret = -1;
	RSUA * p_rua = (RSUA *)argv;
	UA_PACKET packet;
	int samplerate = p_rua->media_info.a_samplerate;

	while (p_rua->rtp_tx)
	{
	    if (p_rua->rtp_pause)
	    {
	        usleep(10*1000);
	        continue;
	    }

		if (hqBufGet(p_rua->media_info.a_queue, (char *)&packet))
		{
			if (packet.data == NULL || packet.size == 0)
			{
				break;
			}
			else if (p_rua->media_info.a_codec == AUDIO_CODEC_AAC)
			{
				sret = rtp_aac_audio_tx(p_rua, packet.data, packet.size, rtsp_get_timestamp(samplerate));
			}
			else
			{
				sret = rtp_audio_tx(p_rua, packet.data, packet.size, rtsp_get_timestamp(samplerate));
			}

			free(packet.buff);

#ifdef RTSP_REPLAY
            if (p_rua->replay && !p_rua->rate_control)
            {
                usleep(10*1000);
                continue;
            }
#endif

			if (packet.waitnext)
			{
				usleep(1000000 * packet.nbsamples / samplerate);
			}
		}
		else
		{
		    // should be not to here
		    log_print(LOG_ERR, "%s, hqBufGet failed\r\n", __FUNCTION__);
		    break;
		}
	}

	rtsp_media_clear_queue(p_rua->media_info.a_queue);

	p_rua->media_info.a_thread = 0;

	UNUSED( sret );
	return NULL;
}

#ifdef RTSP_METADATA

void * rtsp_media_metadata_thread(void * argv)
{
	RSUA * p_rua = (RSUA *)argv;
	//static int count = 0;
	//bool bExit = false;
	//static int i = 0;

	int fps = 1000000 / 30;
	int dt_rate = 0;
	while (p_rua->rtp_tx)
	{
	    if (p_rua->rtp_pause)
	    {
	        usleep(10*1000);
	        continue;
	    }


		UA_PACKET packet;
		try
		{
			if (hqBufGet(p_rua->media_info.md_queue, (char*)&packet))
			{
				if (packet.data == NULL || packet.size == 0)
				{
					//printf("rtsp_media_metadata_thread received empty packet");
					// 종료하라는 신호가 오면
					break;
				}
				else
				{
					int ret = rtp_metadata_tx(p_rua, packet.data, packet.size, rtsp_get_timestamp(90000));
					UNUSED( ret );
					//printf("rtp_metadata_tx ret is %d\n", ret);
				}
				free(packet.buff);
			}
			else
			{
				int slp = fmax(fps - dt_rate * 1000, 0);
				usleep(slp); // 30fps
				//printf("rtsp_media_metadata_thread get failed");

			}

#ifdef RTSP_REPLAY
			if (p_rua->replay && !p_rua->rate_control)
			{
				usleep(10 * 1000);
				continue;
			}
#endif

		}
		catch (...)
		{
			printf("media metadata thread an exception occured while trying to get an item from the queue");
		}

	}
	//printf("signal media metadata thread to stop");
	p_rua->media_info.m_thread = 0;

	return NULL;
}
#endif

void  rtsp_media_put_video(RSUA * p_rua, uint8 *data, int size, int waitnext = 1)
{
	UA_PACKET packet;

	if (!p_rua->rtp_tx)
	{
		return;
	}

	if (data && size > 0)
	{
		packet.buff = (uint8*)malloc(size+40);
		packet.data = packet.buff + 40;

		if (packet.buff)
		{
			memcpy(packet.data, data, size);
			packet.size = size;
			packet.waitnext = waitnext;

			if (hqBufPut(p_rua->media_info.v_queue, (char *)&packet) == FALSE)
			{
				log_print(LOG_ERR, "rtsp_media_put_video failed to put item into video queue\r\n");
				free(packet.buff);
			}
		}
	}
	else
	{
		packet.data = NULL;
		packet.size = 0;
		packet.waitnext = waitnext;

		if (!hqBufPut(p_rua->media_info.v_queue, (char*)&packet))
			log_print(LOG_ERR, "rtsp_media_put_video failed to put empty item into video queue\r\n");
	}
}

void rtsp_media_put_audio(RSUA * p_rua, uint8 *data, int size, int nbsamples, int waitnext = 1)
{
	UA_PACKET packet;

	if (!p_rua->rtp_tx)
	{
		return;
	}

	if (data && size > 0)
	{
		packet.buff = (uint8*)malloc(size+40); // skip forward header
		packet.data = packet.buff+40;

		if (packet.buff)
		{
			memcpy(packet.data, data, size);
			packet.size = size;
			packet.nbsamples = nbsamples;
			packet.waitnext = waitnext;

			if (hqBufPut(p_rua->media_info.a_queue, (char *)&packet) == FALSE)
			{
				log_print(LOG_ERR, "rtsp_media_put_audio failed to put item into audio queue\r\n");
				free(packet.buff);
			}
		}
	}
	else
	{
		packet.data = NULL;
		packet.size = 0;
		packet.nbsamples = 0;
		packet.waitnext = waitnext;

		if (hqBufPut(p_rua->media_info.a_queue, (char*)&packet) == FALSE)
		{
			log_print(LOG_ERR, "rtsp_media_put_audio failed to put empty item into audio queue\r\n");
		}
	}
}

void  rtsp_media_put_metadata(RSUA* p_rua, uint8* data, int size, int waitnext = 1)
{
	UA_PACKET packet;
	//printf("rtsp_media_put_metadata\n");
	try
	{
		if (!p_rua->rtp_tx)
		{
			return;
		}
		//static int i = 0;
		if (data && size > 0)
		{
			packet.buff = (uint8*)malloc(size + 40);
			packet.data = packet.buff + 40;
			//packet.buff = (uint8*)malloc(size + 40);
			//packet.data = packet.buff + 40;
			//printf("put md_queue %d\n", i++);
			//printf("send metadata(%d)[%d] %s\n", i, size, packet.data);

			if (packet.buff)
			{
				memcpy(packet.data, data, size);
				packet.size = size;
				packet.waitnext = waitnext;
				//std::thread::id threadId = std::this_thread::get_id();
				//std::cout << threadId << " ; rtsp_media_put_metadata" << std::endl;
				if (hqBufPut(p_rua->media_info.md_queue, (char*)&packet) == FALSE)
				{
					log_print(LOG_ERR, "rtsp_media_put_metadata failed to put item into metadata queue\r\n");
					free(packet.buff);
				}
			}

		}
		else
		{
			packet.data = NULL;
			packet.size = 0;
			packet.waitnext = waitnext;

			if (hqBufPut(p_rua->media_info.md_queue, (char*)&packet) == FALSE)
			{
				log_print(LOG_ERR, "rtsp_media_put_metadata failed to put empty item into metadata queue\r\n");
			}
		}
	}
	catch (...)
	{
		printf("rtsp_media_put_metadata unknown exception occured");
	}
}

void rtsp_send_epmty_packet(HQUEUE * hq)
{
	UA_PACKET packet;
	memset(&packet, 0, sizeof(packet));

	if (!hqBufPut(hq, (char*)&packet))
		printf("failed to put empty rtsp packet in queue\n");
}

void rtsp_media_free_queue(RSUA * p_rua, const BOOL b_terminate_queue = TRUE )
{
	if( p_rua->media_info.has_video && p_rua->media_info.v_queue )
		rtsp_send_epmty_packet(p_rua->media_info.v_queue);

	if( p_rua->media_info.has_audio && p_rua->media_info.a_queue )
		rtsp_send_epmty_packet(p_rua->media_info.a_queue);

#ifdef RTSP_METADATA
	if( p_rua->media_info.md_queue )
		rtsp_send_epmty_packet(p_rua->media_info.md_queue);
#endif

	// 비디오 전송 쓰레드 기다림
	while( p_rua->media_info.v_thread ) { usleep( 10 * 1000 ); }

	// 비디오 전송 큐 비우기
	rtsp_media_clear_queue( p_rua->media_info.v_queue );
	// 비디오 전송 큐 지우기
	if( b_terminate_queue == TRUE ) {
		hqDelete(p_rua->media_info.v_queue);
		p_rua->media_info.v_queue = NULL;
	}

	// 오디오 전송 쓰레드 기다림
	while( p_rua->media_info.a_thread ) { usleep( 10 * 1000 ); }

	// 오디오 전송 큐 비우기
	rtsp_media_clear_queue(p_rua->media_info.a_queue);
	// 오디오 전송 큐 지우기
	if( b_terminate_queue == TRUE ) {
		hqDelete(p_rua->media_info.a_queue);
		p_rua->media_info.a_queue = NULL;
	}

#ifdef RTSP_METADATA
	// 메타데이터 전송 쓰레드 기다림
	while( p_rua->media_info.m_thread ) { usleep( 10 * 1000 ); }

	// 메타데이터 전송큐 비우기
	rtsp_media_clear_queue( p_rua->media_info.md_queue );
	// 메타데이터 전송큐 지우기
	if( b_terminate_queue == TRUE ) {
		hqDelete( p_rua->media_info.md_queue );
		p_rua->media_info.md_queue = NULL;
	}
#endif
}

#ifdef PROXY
void rtsp_media_proxy_callback(uint8 * data, int size, int type, void * pUserdata)
{
	//if (type == DATA_TYPE_METADATA)
	//	printf("rtsp_media_proxy_callback metadata");
	//if (type == DATA_TYPE_VIDEO)
	//	printf("rtsp_media_proxy_callback video");
	try
	{
		RSUA* p_rua = (RSUA*)pUserdata;

		if (type == DATA_TYPE_AUDIO && p_rua->a_setup)
		{
			rtsp_media_put_audio((RSUA*)pUserdata, data, size, 0, 0);
		}
		else if (type == DATA_TYPE_VIDEO && p_rua->v_setup)
		{
			//printf("rtsp_media_proxy_callback received video data\n");
			//현재 상황은 카메라 쓰레드는 RTSP Teardown 요청 받기 전에 그 쓰레드와 관련된 queue 다 Deadlock 걸립니다 (RTSP proxy datacallback, v_queue 등등)
			if (size <= 0)
			{
				//printf("RTP video packet size for RTSP client is 0, skip this packet to prevent deadlock\n");
				return;
			}
			rtsp_media_put_video((RSUA*)pUserdata, data, size, 0);
		}
		else if (type == DATA_TYPE_METADATA && p_rua->v_setup)
		{
			rtsp_media_put_metadata((RSUA*)pUserdata, data, size, 0);
		}
	}
	catch (...)
	{
		printf("rtsp_media_proxy_callback unknown exception occured\n");
	}

}

void rtsp_media_proxy_send_thread(RSUA * p_rua)
{
    CRtspProxy * p_proxy = p_rua->media_info.proxy;
    if (NULL == p_proxy)
    {
        log_print(LOG_ERR, "%s, proxy object is null\r\n", __FUNCTION__);
        return;
    }

	if( p_rua->media_info.has_video && p_rua->v_setup ) {
		if( p_rua->media_info.v_queue == NULL )
			p_rua->media_info.v_queue = hqCreate( 60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT );
		p_rua->media_info.v_thread = sys_os_create_thread( (void *)rtsp_media_video_thread, (void*)p_rua );
	}

	if( p_rua->media_info.has_audio && p_rua->a_setup ) {
		if( p_rua->media_info.a_queue == NULL )
			p_rua->media_info.a_queue = hqCreate( 60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT );
		p_rua->media_info.a_thread = sys_os_create_thread( (void *)rtsp_media_audio_thread, (void*)p_rua );
	}

#ifdef RTSP_METADATA
	if( p_rua->media_info.has_video && p_rua->v_setup ) {
		if( p_rua->media_info.md_queue == NULL )
			p_rua->media_info.md_queue = hqCreate(10000, sizeof(UA_PACKET), HQ_PUT_WAIT);
		p_rua->media_info.m_thread = sys_os_create_thread( (void*)rtsp_media_metadata_thread, (void*)p_rua );
	}
#endif

	p_proxy->addCallback(rtsp_media_proxy_callback, p_rua);
	// wait sub-threads
	while( p_rua->rtp_tx )
	{
		usleep( 20 * 1000 );
	}
    p_proxy->delCallback(rtsp_media_proxy_callback, p_rua);

	rtsp_media_free_queue(p_rua, FALSE); // not delete hqueue, just clear hqueue
}
#endif // end of PROXY

#ifdef PUSHER
void rtsp_media_pusher_callback(uint8 * data, int size, int type, void * pUserdata)
{
	RSUA * p_rua = (RSUA *) pUserdata;

	if (type == DATA_TYPE_AUDIO && p_rua->a_setup)
	{
		rtsp_media_put_audio((RSUA *) pUserdata, data, size, 0, 0);
	}
	else if (type == DATA_TYPE_VIDEO && p_rua->v_setup)
	{
		rtsp_media_put_video((RSUA *) pUserdata, data, size, 0);
	}
}

void rtsp_media_pusher_send_thread(RSUA * p_rua)
{
    CRtspPusher * p_pusher = p_rua->media_info.pusher;
    if (NULL == p_pusher)
    {
        log_print(LOG_ERR, "%s, pusher object is null\r\n", __FUNCTION__);
        return;
    }

	if (p_rua->media_info.has_video && p_rua->v_setup)
	{
		p_rua->media_info.v_queue = hqCreate(60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT);
		p_rua->media_info.v_thread = sys_os_create_thread((void *)rtsp_media_video_thread, (void*)p_rua);
	}

	if (p_rua->media_info.has_audio && p_rua->a_setup)
	{
		p_rua->media_info.a_queue = hqCreate(60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT);
		p_rua->media_info.a_thread = sys_os_create_thread((void *)rtsp_media_audio_thread, (void*)p_rua);
	}

	p_pusher->addCallback(rtsp_media_pusher_callback, p_rua);

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

    p_pusher->delCallback(rtsp_media_pusher_callback, p_rua);

	rtsp_media_free_queue(p_rua);
}
#endif // end of PUSHER


#ifdef RTSP_FILE
void rtsp_media_demux_callback(uint8 * data, int size, int type, int nbsamples, BOOL waitnext, void * pUserdata)
{
	RSUA * p_rua = (RSUA *) pUserdata;

	if (type == DATA_TYPE_AUDIO && p_rua->a_setup)
	{
		rtsp_media_put_audio(p_rua, data, size, nbsamples, waitnext);
	}
	else if (type == DATA_TYPE_VIDEO && p_rua->v_setup)
	{
		rtsp_media_put_video(p_rua, data, size, waitnext);
	}
}

void rtsp_media_file_send_thread(RSUA * p_rua)
{
	CFileDemux * pDemux = p_rua->media_info.file_demuxer;

	pDemux->setCallback(rtsp_media_demux_callback, p_rua);

	if (p_rua->media_info.has_video && p_rua->v_setup)
	{
		p_rua->media_info.v_queue = hqCreate(60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT);
		p_rua->media_info.v_thread = sys_os_create_thread((void *)rtsp_media_video_thread, (void*)p_rua);
	}

	if (p_rua->media_info.has_audio && p_rua->a_setup)
	{
		p_rua->media_info.a_queue = hqCreate(60, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT);
		p_rua->media_info.a_thread = sys_os_create_thread((void *)rtsp_media_audio_thread, (void*)p_rua);
	}

#ifdef RTSP_METADATA
	if (p_rua->media_info.metadata && p_rua->m_setup)
	{
		p_rua->media_info.md_queue = hqCreate(10000, sizeof(UA_PACKET), HQ_PUT_WAIT);
		//printf("rtsp_media_device_send_thread create rtsp_media_metadata_thread\n");
		p_rua->media_info.m_thread = sys_os_create_thread((void *)rtsp_media_metadata_thread, (void*)p_rua);
	}
#endif

	while (p_rua->rtp_tx)
	{
	    if (p_rua->rtp_pause)
	    {
	        usleep(10*1000);
	        continue;
	    }

		if (pDemux->readFrame() == FALSE)
		{
			break;
		}

		usleep(10*1000);
	}

	delete pDemux;
	p_rua->media_info.file_demuxer = NULL;

	rtsp_media_free_queue(p_rua);

}
#endif // end of RTSP_FILE

#ifdef RTSP_DEVICE

void rtsp_media_video_callback(uint8 * data, int size, void * pUserdata)
{
	rtsp_media_put_video((RSUA *)pUserdata, data, size, 0);
}

void rtsp_media_audio_callback(uint8 * data, int size, int nbsamples, void * pUserdata)
{
	rtsp_media_put_audio((RSUA *)pUserdata, data, size, nbsamples, 0);
}

void rtsp_media_video_capture(RSUA * p_rua)
{
	CVideoCapture * capture = p_rua->media_info.video_capture;
	if (NULL == capture)
	{
		log_print(LOG_ERR, "%s, get video capture instance (%d) failed\r\n", __FUNCTION__, p_rua->media_info.v_index);
		return;
	}

	p_rua->media_info.v_queue = hqCreate(60, sizeof(UA_PACKET), HQ_GET_WAIT | HQ_PUT_WAIT);
	p_rua->media_info.v_thread = sys_os_create_thread((void *)rtsp_media_video_thread, (void*)p_rua);

	capture->addCallback(rtsp_media_video_callback, p_rua);
	capture->startCapture();

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

	capture->delCallback(rtsp_media_video_callback, p_rua);
	capture->freeInstance(p_rua->media_info.v_index);

	rtsp_send_epmty_packet(p_rua->media_info.v_queue);

	while (p_rua->media_info.v_thread)
	{
		usleep(10*1000);
	}

	rtsp_media_clear_queue(p_rua->media_info.v_queue);

	hqDelete(p_rua->media_info.v_queue);
	p_rua->media_info.v_queue = NULL;
}

void rtsp_media_audio_capture(RSUA * p_rua)
{
	CAudioCapture * capture = p_rua->media_info.audio_capture;
	if (NULL == capture)
	{
		log_print(LOG_ERR, "%s, get audio capture instace (%d) failed\r\n", __FUNCTION__, p_rua->media_info.a_index);
		return;
	}

	p_rua->media_info.a_queue = hqCreate(60, sizeof(UA_PACKET), HQ_GET_WAIT | HQ_PUT_WAIT);
	p_rua->media_info.a_thread = sys_os_create_thread((void *)rtsp_media_audio_thread, (void*)p_rua);

	capture->addCallback(rtsp_media_audio_callback, p_rua);
	capture->startCapture();

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

	capture->delCallback(rtsp_media_audio_callback, p_rua);
	capture->freeInstance(p_rua->media_info.a_index);

	rtsp_send_epmty_packet(p_rua->media_info.a_queue);

	while (p_rua->media_info.a_thread)
	{
		usleep(10*1000);
	}

	rtsp_media_clear_queue(p_rua->media_info.a_queue);

	hqDelete(p_rua->media_info.a_queue);
	p_rua->media_info.a_queue = NULL;
}

void rtsp_media_screen_capture(RSUA * p_rua)
{
	CScreenCapture * capture = p_rua->media_info.screen_capture;

	p_rua->media_info.v_queue = hqCreate(60, sizeof(UA_PACKET), HQ_GET_WAIT | HQ_PUT_WAIT);
	p_rua->media_info.v_thread = sys_os_create_thread((void *)rtsp_media_video_thread, (void*)p_rua);

	capture->addCallback(rtsp_media_video_callback, p_rua);
	capture->startCapture();

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

	capture->delCallback(rtsp_media_video_callback, p_rua);
	capture->freeInstance(p_rua->media_info.v_index);

	rtsp_send_epmty_packet(p_rua->media_info.v_queue);

	while (p_rua->media_info.v_thread)
	{
		usleep(10*1000);
	}

	rtsp_media_clear_queue(p_rua->media_info.v_queue);

	hqDelete(p_rua->media_info.v_queue);
	p_rua->media_info.v_queue = NULL;
}

void * rtsp_media_audio_capture_thread(void * argv)
{
	RSUA * p_rua = (RSUA *)argv;

	rtsp_media_audio_capture(p_rua);

	p_rua->audio_thread = 0;

	return NULL;
}

void rtsp_media_device_send_thread(RSUA * p_rua)
{
#ifdef RTSP_METADATA
	if (p_rua->media_info.metadata && p_rua->m_setup)
	{
		p_rua->media_info.md_queue = hqCreate(10000, sizeof(UA_PACKET), HQ_PUT_WAIT);
		//printf("rtsp_media_device_send_thread create rtsp_media_metadata_thread\n");
		p_rua->media_info.m_thread = sys_os_create_thread((void *)rtsp_media_metadata_thread, (void*)p_rua);
	}
#endif

	if (p_rua->media_info.has_video && p_rua->v_setup)
	{
		if (p_rua->media_info.has_audio)
		{
			p_rua->audio_thread = sys_os_create_thread((void *)rtsp_media_audio_capture_thread, (void *)p_rua);
		}

		if (p_rua->media_info.screen)
		{
			rtsp_media_screen_capture(p_rua);
		}
		else
		{
			rtsp_media_video_capture(p_rua);
		}

		if (p_rua->media_info.has_audio)
		{
			// wait audio capture thread exit ...
			while (p_rua->audio_thread)
			{
				usleep(200*000);
			}
		}
	}
	else if (p_rua->media_info.has_audio && p_rua->a_setup)
	{
		rtsp_media_audio_capture(p_rua);
	}
}

#endif // RTSP_DEVICE

#ifdef RTSP_LIVE
void rtsp_media_live_video_callback(uint8 * data, int size, void * pUserdata)
{
	rtsp_media_put_video((RSUA *)pUserdata, data, size, 0);
}

void rtsp_media_live_audio_callback(uint8 * data, int size, int nbsamples, void * pUserdata)
{
	rtsp_media_put_audio((RSUA *)pUserdata, data, size, nbsamples, 0);
}

void rtsp_media_live_video_capture(RSUA * p_rua)
{
	CLiveVideo * capture = p_rua->media_info.live_video;
    if (NULL == capture)
    {
        log_print(LOG_ERR, "%s, capture object is null\r\n", __FUNCTION__);
        return;
    }

    p_rua->media_info.v_queue = hqCreate(60, sizeof(UA_PACKET), HQ_GET_WAIT | HQ_PUT_WAIT);
	p_rua->media_info.v_thread = sys_os_create_thread((void *)rtsp_media_video_thread, (void*)p_rua);

	capture->addCallback(rtsp_media_live_video_callback, p_rua);
	capture->startCapture();

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

	capture->delCallback(rtsp_media_live_video_callback, p_rua);
	capture->freeInstance(p_rua->media_info.v_index);

	rtsp_send_epmty_packet(p_rua->media_info.v_queue);

	while (p_rua->media_info.v_thread)
	{
		usleep(10*1000);
	}

	rtsp_media_clear_queue(p_rua->media_info.v_queue);

	hqDelete(p_rua->media_info.v_queue);
	p_rua->media_info.v_queue = NULL;
}

void rtsp_media_live_audio_capture(RSUA * p_rua)
{
	CLiveAudio * capture = p_rua->media_info.live_audio;
	if (NULL == capture)
	{
		log_print(LOG_ERR, "%s, get audio capture instace (%d) failed\r\n", __FUNCTION__, p_rua->media_info.a_index);
		return;
	}

	p_rua->media_info.a_queue = hqCreate(60, sizeof(UA_PACKET), HQ_GET_WAIT | HQ_PUT_WAIT);
	p_rua->media_info.a_thread = sys_os_create_thread((void *)rtsp_media_audio_thread, (void*)p_rua);

	capture->addCallback(rtsp_media_live_audio_callback, p_rua);
	capture->startCapture();

	while (p_rua->rtp_tx)
	{
		usleep(200*1000);
	}

	capture->delCallback(rtsp_media_live_audio_callback, p_rua);
	capture->freeInstance(p_rua->media_info.a_index);

	rtsp_send_epmty_packet(p_rua->media_info.a_queue);

	while (p_rua->media_info.a_thread)
	{
		usleep(10*1000);
	}

	rtsp_media_clear_queue(p_rua->media_info.a_queue);

	hqDelete(p_rua->media_info.a_queue);
	p_rua->media_info.a_queue = NULL;
}

void * rtsp_media_live_audio_capture_thread(void * argv)
{
	RSUA * p_rua = (RSUA *)argv;

	rtsp_media_live_audio_capture(p_rua);

	p_rua->audio_thread = NULL;

	return NULL;
}

void rtsp_media_live_send_thread(RSUA * p_rua)
{
#ifdef RTSP_METADATA
	if (p_rua->media_info.metadata && p_rua->m_setup)
	{
		p_rua->media_info.md_queue = hqCreate(10000, sizeof(UA_PACKET), HQ_PUT_WAIT | HQ_GET_WAIT);
		p_rua->media_info.m_thread = sys_os_create_thread((void *)rtsp_media_metadata_thread, (void*)p_rua);
	}
#endif

	if (p_rua->media_info.has_video && p_rua->v_setup)
	{
		if (p_rua->media_info.has_audio)
		{
			p_rua->audio_thread = sys_os_create_thread((void *)rtsp_media_live_audio_capture_thread, (void *)p_rua);
		}

		rtsp_media_live_video_capture(p_rua);

		if (p_rua->media_info.has_audio)
		{
			// wait audio capture thread exit ...
			while (p_rua->audio_thread)
			{
				usleep(200*000);
			}
		}
	}
	else if (p_rua->media_info.has_audio && p_rua->a_setup)
	{
		rtsp_media_live_audio_capture(p_rua);
	}
}
#endif

void rtsp_terminate_queue( void* rua )
{
	RSUA* p_rua = (RSUA*)rua;
	rtsp_media_free_queue( p_rua, TRUE );
}

void rtsp_media_send_thread(void * rua)
{
	//printf("rtsp_media_send_thread\n");
	RSUA * p_rua = (RSUA *)rua;

#ifdef PROXY
    if (p_rua->media_info.is_proxy)
    {
		// SHY : If you PLAY, here in
        rtsp_media_proxy_send_thread(p_rua);
    }
    else
#endif

#ifdef PUSHER
    if (p_rua->media_info.is_pusher)
    {
		rtsp_media_pusher_send_thread(p_rua);
    }
    else
#endif

#ifdef RTSP_FILE
	if (p_rua->media_info.media_file)
	{
		rtsp_media_file_send_thread(p_rua);
	}
	else
#endif

#ifdef RTSP_LIVE
	if (p_rua->media_info.is_live)
	{
		rtsp_media_live_send_thread(p_rua);
	}
	else
#endif

#ifdef RTSP_DEVICE
	{
		rtsp_media_device_send_thread(p_rua);
	}
#else
	{
	}
#endif

	p_rua->rtp_thread = 0;
}






