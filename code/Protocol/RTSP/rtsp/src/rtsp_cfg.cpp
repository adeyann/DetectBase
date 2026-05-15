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
#include "rtsp_cfg.h"
#include "xml_node.h"
#include "media_format.h"

/***********************************************************/

// the default configuration file name
#define RTSP_DEF_CFG	"config.xml"

// global rtsp configuration variable
RTSP_CFG g_rtsp_cfg;

/***********************************************************/

RTSP_USER * rtsp_get_idle_user()
{
    int i;

	for (i = 0; i < MAX_USERS; i++)
	{
		if (g_rtsp_cfg.users[i].Username[0] == '\0')
		{
			return &g_rtsp_cfg.users[i];
		}
	}

	return NULL;
}

BOOL rtsp_add_user(RTSP_USER * p_user)
{
    RTSP_USER * p_idle_user;

	p_idle_user = rtsp_get_idle_user();
	if (p_idle_user)
	{
		memcpy(p_idle_user, p_user, sizeof(RTSP_USER));
		return TRUE;
	}

	return FALSE;
}

RTSP_USER * rtsp_find_user(const char * username)
{
    int i;

	for (i = 0; i < MAX_USERS; i++)
	{
		if (strcmp(g_rtsp_cfg.users[i].Username, username) == 0)
		{
			return &g_rtsp_cfg.users[i];
		}
	}

	return NULL;
}

const char * rtsp_get_user_pass(const char * username)
{
    RTSP_USER * p_user;

    if (NULL == username || strlen(username) == 0)
    {
        return NULL;
    }

    p_user = rtsp_find_user(username);
	if (NULL != p_user)
	{
	    return p_user->Password;
	}

	return NULL;
}

RTSP_OUTPUT * rtsp_add_output(RTSP_OUTPUT ** p_output)
{
	RTSP_OUTPUT * p_tmp;
	RTSP_OUTPUT * p_new_output = (RTSP_OUTPUT *) malloc(sizeof(RTSP_OUTPUT));
	if (NULL == p_new_output)
	{
		return NULL;
	}

	memset(p_new_output, 0, sizeof(RTSP_OUTPUT));

	p_tmp = *p_output;
	if (NULL == p_tmp)
	{
		*p_output = p_new_output;
	}
	else
	{
		while (p_tmp && p_tmp->next) p_tmp = p_tmp->next;

		p_tmp->next = p_new_output;
	}

	return p_new_output;
}

void rtsp_free_outputs(RTSP_OUTPUT ** p_output)
{
	RTSP_OUTPUT * p_next;
	RTSP_OUTPUT * p_tmp = *p_output;

	while (p_tmp)
	{
		p_next = p_tmp->next;
		free(p_tmp);
		p_tmp = p_next;
	}

	*p_output = NULL;
}

void rtsp_camera_list(std::vector<int>& cameraIds)
{
	RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
	while (p_proxy)
	{
		cameraIds.push_back(std::stoi(p_proxy->cfg.suffix));
		p_proxy = p_proxy->next;
	}
}


/***********************************************************/
BOOL rtsp_parse_user(XMLN * p_node, RTSP_USER * p_user)
{
	XMLN * p_username;
    XMLN * p_password;

	p_username = xml_node_get(p_node, "username");
	if (p_username && p_username->data)
	{
		strncpy(p_user->Username, p_username->data, sizeof(p_user->Username)-1);
	}
	else
	{
		return FALSE;
	}

	p_password = xml_node_get(p_node, "password");
	if (p_password && p_password->data)
	{
		strncpy(p_user->Password, p_password->data, sizeof(p_user->Password)-1);
	}

	return TRUE;
}

int rtsp_parse_video_codec(const char * buff)
{
	if (stricmp(buff, "H264") == 0)
	{
		return VIDEO_CODEC_H264;
	}
	else if (stricmp(buff, "H265") == 0)
	{
		return VIDEO_CODEC_H265;
	}
	else if (stricmp(buff, "MP4") == 0)
	{
		return VIDEO_CODEC_MP4;
	}
	else if (stricmp(buff, "JPEG") == 0)
	{
		return VIDEO_CODEC_JPEG;
	}

	return VIDEO_CODEC_NONE;
}

int rtsp_parse_audio_codec(const char * buff)
{
	if (stricmp(buff, "G711") == 0)
	{
		return AUDIO_CODEC_G711A;
	}
	else if (stricmp(buff, "G711A") == 0)
	{
		return AUDIO_CODEC_G711A;
	}
	else if (stricmp(buff, "G711U") == 0)
	{
		return AUDIO_CODEC_G711U;
	}
	else if (stricmp(buff, "G726") == 0)
	{
		return AUDIO_CODEC_G726;
	}
	else if (stricmp(buff, "AAC") == 0)
	{
		return AUDIO_CODEC_AAC;
	}
	else if (stricmp(buff, "G722") == 0)
	{
		return AUDIO_CODEC_G722;
	}
	else if (stricmp(buff, "OPUS") == 0)
	{
		return AUDIO_CODEC_OPUS;
	}

	return AUDIO_CODEC_NONE;
}

BOOL rtsp_parse_video_info(XMLN * p_node, RTSP_V_INFO * p_info)
{
	XMLN * p_codec;
	XMLN * p_width;
	XMLN * p_height;
	XMLN * p_framerate;
	XMLN * p_bitrate;

	p_codec = xml_node_get(p_node, "codec");
	if (p_codec && p_codec->data)
	{
		p_info->codec = rtsp_parse_video_codec(p_codec->data);
	}

	p_width = xml_node_get(p_node, "width");
	if (p_width && p_width->data)
	{
		p_info->width = atoi(p_width->data);

		if (p_info->width < 0)
		{
			p_info->width = 0;
		}
	}

	p_height = xml_node_get(p_node, "height");
	if (p_height && p_height->data)
	{
		p_info->height = atoi(p_height->data);

		if (p_info->height < 0)
		{
			p_info->height = 0;
		}
	}

	p_framerate = xml_node_get(p_node, "framerate");
	if (p_framerate && p_framerate->data)
	{
		p_info->framerate = atoi(p_framerate->data);

		if (p_info->framerate < 0)
		{
			p_info->framerate = 0;
		}
	}

	p_bitrate = xml_node_get(p_node, "bitrate");
	if (p_bitrate && p_bitrate->data)
	{
		p_info->bitrate = atoi(p_bitrate->data);

		if (p_info->bitrate < 0)
		{
			p_info->bitrate = 0;
		}
	}

	return TRUE;
}

BOOL rtsp_parse_audio_info(XMLN * p_node, RTSP_A_INFO * p_info)
{
	XMLN * p_codec;
	XMLN * p_samplerate;
	XMLN * p_channels;
	XMLN * p_bitrate;

	p_codec = xml_node_get(p_node, "codec");
	if (p_codec && p_codec->data)
	{
		p_info->codec = rtsp_parse_audio_codec(p_codec->data);
	}

	p_samplerate = xml_node_get(p_node, "samplerate");
	if (p_samplerate && p_samplerate->data)
	{
		p_info->samplerate = atoi(p_samplerate->data);

		if (p_info->samplerate < 0)
		{
			p_info->samplerate = 0;
		}
	}

	p_channels = xml_node_get(p_node, "channels");
	if (p_channels && p_channels->data)
	{
		p_info->channels = atoi(p_channels->data);

		if (p_info->channels < 0)
		{
			p_info->channels = 0;
		}
		else if (p_info->channels > 2)
		{
			p_info->channels = 2;
		}
	}

	p_bitrate = xml_node_get(p_node, "bitrate");
	if (p_bitrate && p_bitrate->data)
	{
		p_info->bitrate = atoi(p_bitrate->data);

		if (p_info->bitrate < 0)
		{
			p_info->bitrate = 0;
		}
	}

	return TRUE;
}

BOOL rtsp_parse_output(XMLN * p_node, RTSP_OUTPUT * p_output)
{
	XMLN * p_url;
	XMLN * p_video;
	XMLN * p_audio;

	p_url = xml_node_get(p_node, "url");
	if (p_url && p_url->data)
	{
		strncpy(p_output->url, p_url->data, sizeof(p_output->url)-1);
	}

	p_video = xml_node_get(p_node, "video");
	if (p_video)
	{
		rtsp_parse_video_info(p_video, &p_output->v_info);
	}

	p_audio = xml_node_get(p_node, "audio");
	if (p_audio)
	{
		rtsp_parse_audio_info(p_audio, &p_output->a_info);
	}

	return TRUE;
}

#ifdef PROXY
void xml_unescape(char* str) {
    char* read = str;
    char* write = str;

    while (*read) {
        if (strncmp(read, "&amp;", 5) == 0) {
            *write++ = '&';
            read += 5;
        } else if (strncmp(read, "&lt;", 4) == 0) {
            *write++ = '<';
            read += 4;
        } else if (strncmp(read, "&gt;", 4) == 0) {
            *write++ = '>';
            read += 4;
        } else if (strncmp(read, "&quot;", 6) == 0) {
            *write++ = '"';
            read += 6;
        } else if (strncmp(read, "&apos;", 6) == 0) {
            *write++ = '\'';
            read += 6;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

BOOL rtsp_parse_proxy_cfg(XMLN * p_node, PROXY_CFG * p_proxy)
{
    XMLN * p_suffix;
	XMLN * p_url;
	XMLN* p_ip;
	XMLN * p_user;
	XMLN * p_pass;
	XMLN * p_output;
	XMLN * p_port;
	XMLN * p_group;
	XMLN * p_vms;
	XMLN * p_guid;

    p_suffix = xml_node_get(p_node, "suffix");
	if (p_suffix && p_suffix->data)
	{
		strncpy(p_proxy->suffix, p_suffix->data, sizeof(p_proxy->suffix)-1);
	}
	else
	{
	    return FALSE;
	}

	p_url = xml_node_get(p_node, "url");
	if (p_url && p_url->data)
	{
		strncpy(p_proxy->url, p_url->data, sizeof(p_proxy->url)-1);
		xml_unescape(p_proxy->url);  // 🔧 복원 처리 추가
	}
	else
	{
	    return FALSE;
	}

	p_user = xml_node_get(p_node, "user");
	if (p_user && p_user->data)
	{
		strncpy(p_proxy->user, p_user->data, sizeof(p_proxy->user)-1);
	}

	p_pass = xml_node_get(p_node, "pass");
	if (p_pass && p_pass->data)
	{
		strncpy(p_proxy->pass, p_pass->data, sizeof(p_proxy->pass)-1);
	}

	p_ip = xml_node_get(p_node, "ip");
	if (p_ip && p_ip->data)
	{
		strncpy(p_proxy->ip, p_ip->data, sizeof(p_proxy->ip) - 1);
	}
	p_port = xml_node_get(p_node, "port");
	if (p_port && p_port->data)
	{
		p_proxy->port = atoi(p_port->data);
	}
	p_group = xml_node_get(p_node, "group");
	if (p_group && p_group->data)
	{
		strncpy(p_proxy->group, p_group->data, sizeof(p_proxy->group) - 1);
	}
	p_vms = xml_node_get(p_node, "vms");
	if (p_vms && p_vms->data)
	{
		strncpy(p_proxy->vmsName, p_vms->data, sizeof(p_proxy->vmsName) - 1);
	}


	p_output = xml_node_get(p_node, "output");
	if (p_output)
	{
		p_proxy->has_output = rtsp_parse_output(p_output, &p_proxy->output);
	}

	p_guid = xml_node_get(p_node, "guid");
	if (p_guid && p_guid->data)
	{
		strncpy(p_proxy->guid, p_guid->data, sizeof(p_proxy->guid) - 1);
	}

	return TRUE;
}
#endif // end of  PROXY

#ifdef PUSHER
int rtsp_parse_transfer_mode(const char * str)
{
	if (strcasecmp(str, "TCP") == 0)
	{
		return TRANSFER_MODE_TCP;
	}
	else if (strcasecmp(str, "UDP") == 0)
	{
		return TRANSFER_MODE_UDP;
	}
	else if (strcasecmp(str, "RTSP") == 0)
	{
		return TRANSFER_MODE_RTSP;
	}

	return -1;
}

BOOL rtsp_parse_pusher_cfg(XMLN * p_node, PUSHER_CFG * p_pusher)
{
	XMLN * p_suffix;
	XMLN * p_video;
	XMLN * p_audio;
	XMLN * p_transfer;
	XMLN * p_output;

    p_suffix = xml_node_get(p_node, "suffix");
	if (p_suffix && p_suffix->data)
	{
		strncpy(p_pusher->suffix, p_suffix->data, sizeof(p_pusher->suffix)-1);
	}
	else
	{
	    return FALSE;
	}

	p_video = xml_node_get(p_node, "video");
	if (p_video)
	{
		p_pusher->has_video = rtsp_parse_video_info(p_video, &p_pusher->v_info);
	}

	p_audio = xml_node_get(p_node, "audio");
	if (p_audio)
	{
		p_pusher->has_audio = rtsp_parse_audio_info(p_audio, &p_pusher->a_info);
	}

	p_transfer = xml_node_get(p_node, "transfer");
	if (p_transfer)
	{
		XMLN * p_mode;
		XMLN * p_ip;
		XMLN * p_vport;
		XMLN * p_aport;

		p_mode = xml_node_get(p_transfer, "mode");
		if (p_mode && p_mode->data)
		{
			p_pusher->transfer.mode = rtsp_parse_transfer_mode(p_mode->data);
		}

		p_ip = xml_node_get(p_transfer, "ip");
		if (p_ip && p_ip->data)
		{
			p_pusher->transfer.ip = inet_addr(p_ip->data);
		}

		p_vport = xml_node_get(p_transfer, "vport");
		if (p_vport && p_vport->data)
		{
			p_pusher->transfer.vport = atoi(p_vport->data);
		}

		p_aport = xml_node_get(p_transfer, "aport");
		if (p_aport && p_aport->data)
		{
			p_pusher->transfer.aport = atoi(p_aport->data);
		}
	}

    p_output = xml_node_get(p_node, "output");
	if (p_output)
	{
	    p_pusher->has_output = rtsp_parse_output(p_output, &p_pusher->output);
	}

	return TRUE;
}
#endif // end of  PUSHER

#ifdef RTSP_BACKCHANNEL
BOOL rtsp_parse_backchannel(XMLN * p_node, RTSP_BC_CFG * p_req)
{
    XMLN * p_codec;
	XMLN * p_samplerate;
	XMLN * p_channels;

	p_codec = xml_node_get(p_node, "codec");
	if (p_codec && p_codec->data)
	{
		p_req->codec = rtsp_parse_audio_codec(p_codec->data);
	}

	p_samplerate = xml_node_get(p_node, "samplerate");
	if (p_samplerate && p_samplerate->data)
	{
		p_req->samplerate = atoi(p_samplerate->data);

		if (p_req->samplerate < 0)
		{
			p_req->samplerate = 8000; // default is 8000
		}
	}

	p_channels = xml_node_get(p_node, "channels");
	if (p_channels && p_channels->data)
	{
		p_req->channels = atoi(p_channels->data);

		if (p_req->channels < 0 || p_req->channels > 2)
		{
			p_req->channels = 1;
		}
	}

	return TRUE;
}
#endif // end of RTSP_BACKCHANNEL

BOOL rtsp_parse_config(char * xml_buff, int rlen)
{
	XMLN * p_node;
	XMLN * p_serverip;
	XMLN * p_serverport;
	XMLN * p_loop_nums;
	XMLN * p_multicast;
	XMLN * p_need_auth;
	XMLN * p_crypt;
	XMLN * p_log_enable;
	XMLN * p_log_level;
#ifdef RTSP_METADATA
	XMLN * p_metadata;
#endif
#ifdef RTSP_OVER_HTTP
    XMLN * p_rtsp_over_http;
    XMLN * p_http_port;
#endif
	XMLN * p_user;
	XMLN * p_output;
#ifdef PROXY
	XMLN * p_proxy;
#endif
#ifdef PUSHER
	XMLN * p_pusher;
#endif
#ifdef RTSP_BACKCHANNEL
    XMLN * p_backchannel;
#endif

	p_node = xxx_hxml_parse(xml_buff, rlen);
	if (NULL == p_node)
	{
		return FALSE;
	}

    p_serverip = xml_node_get(p_node, "serverip");
    while (p_serverip && strcmp(p_serverip->name, "serverip") == 0)
    {
        if (p_serverip->data && strlen(p_serverip->data) > 0)
        {
            int idx = g_rtsp_cfg.serverip_nums;

            strncpy(g_rtsp_cfg.serverip[idx], p_serverip->data, sizeof(g_rtsp_cfg.serverip[idx])-1);

            g_rtsp_cfg.serverip_nums++;
            if (g_rtsp_cfg.serverip_nums >= NET_IF_NUM)
            {
                break;
            }
        }

        p_serverip = p_serverip->next;
    }

	p_serverport = xml_node_get(p_node, "serverport");
	if (p_serverport && p_serverport->data)
	{
		g_rtsp_cfg.serverport = atoi(p_serverport->data);
	}

    p_loop_nums = xml_node_get(p_node, "loop_nums");
	if (p_loop_nums && p_loop_nums->data)
	{
		g_rtsp_cfg.loop_nums = (uint32) atoi(p_loop_nums->data);
	}

    p_multicast = xml_node_get(p_node, "multicast");
	if (p_multicast && p_multicast->data)
	{
		g_rtsp_cfg.multicast = (uint32) atoi(p_multicast->data);
	}

    p_need_auth = xml_node_get(p_node, "need_auth");
	if (p_need_auth && p_need_auth->data)
	{
		g_rtsp_cfg.need_auth = atoi(p_need_auth->data);
	}

	p_crypt = xml_node_get(p_node, "crypt");
	if (p_crypt && p_crypt->data)
	{
		g_rtsp_cfg.crypt = atoi(p_crypt->data);
	}

	p_log_enable = xml_node_get(p_node, "log_enable");
	if (p_log_enable && p_log_enable->data)
	{
		g_rtsp_cfg.log_enable = atoi(p_log_enable->data);
	}

	p_log_level = xml_node_get(p_node, "log_level");
	if (p_log_level && p_log_level->data)
	{
		g_rtsp_cfg.log_level = atoi(p_log_level->data);
	}

#ifdef RTSP_METADATA
	p_metadata = xml_node_get(p_node, "metadata");
	if (p_metadata && p_metadata->data)
	{
		g_rtsp_cfg.metadata = atoi(p_metadata->data);
	}
#endif

#ifdef RTSP_OVER_HTTP
    p_rtsp_over_http = xml_node_get(p_node, "rtsp_over_http");
	if (p_rtsp_over_http && p_rtsp_over_http->data)
	{
		g_rtsp_cfg.rtsp_over_http = atoi(p_rtsp_over_http->data);
	}

	p_http_port = xml_node_get(p_node, "http_port");
	if (p_http_port && p_http_port->data)
	{
		g_rtsp_cfg.http_port = atoi(p_http_port->data);
	}
#endif

	p_user = xml_node_get(p_node, "user");
	while (p_user && strcmp(p_user->name, "user") == 0)
	{
		RTSP_USER user;
		memset(&user, 0, sizeof(user));

    	if (rtsp_parse_user(p_user, &user))
    	{
    		rtsp_add_user(&user);
    	}

    	p_user = p_user->next;
	}

	p_output = xml_node_get(p_node, "output");
	while (p_output && stricmp(p_output->name, "output") == 0)
	{
		RTSP_OUTPUT output;
		memset(&output, 0, sizeof(output));

		if (rtsp_parse_output(p_output, &output))
		{
			RTSP_OUTPUT * p_info = rtsp_add_output(&g_rtsp_cfg.output);
			if (p_info)
			{
				memcpy(p_info, &output, sizeof(RTSP_OUTPUT));
			}
		}

		p_output = p_output->next;
	}

#ifdef PROXY
	p_proxy = xml_node_get(p_node, "proxy");
	while (p_proxy && stricmp(p_proxy->name, "proxy") == 0)
	{
		RTSP_PROXY proxy;
		memset(&proxy, 0, sizeof(proxy));

		if (rtsp_parse_proxy_cfg(p_proxy, &proxy.cfg))
		{
			RTSP_PROXY * p_info = rtsp_add_proxy(&g_rtsp_cfg.proxy);
			if (p_info)
			{
				memcpy(p_info, &proxy, sizeof(RTSP_PROXY));
			}
		}

#ifdef RTSP_DEMO
        break;
#endif

		p_proxy = p_proxy->next;
	}
#endif // end of PROXY

#ifdef PUSHER
	p_pusher = xml_node_get(p_node, "pusher");
	while (p_pusher && stricmp(p_pusher->name, "pusher") == 0)
	{
		RTSP_PUSHER pusher;
		memset(&pusher, 0, sizeof(pusher));

		if (rtsp_parse_pusher_cfg(p_pusher, &pusher.cfg))
		{
			RTSP_PUSHER * p_info = rtsp_add_pusher(&g_rtsp_cfg.pusher);
			if (p_info)
			{
				memcpy(p_info, &pusher, sizeof(RTSP_PUSHER));
			}
		}

#ifdef RTSP_DEMO
		break;
#endif

		p_pusher = p_pusher->next;
	}
#endif // end of PUSHER

#ifdef RTSP_BACKCHANNEL
    p_backchannel = xml_node_get(p_node, "backchannel");
	if (p_backchannel)
	{
		rtsp_parse_backchannel(p_backchannel, &g_rtsp_cfg.backchannel);
	}
#endif

	xml_node_del(p_node);

	return TRUE;
}

/**
 * read configuration file
 *
 * @param config the configuration file name
 * @return TRUE on success, FALSE on error
 */
BOOL rtsp_read_config(const char * config)
{
	BOOL ret = FALSE;
	int len, rlen;
	FILE * fp = NULL;
	char * xml_buff = NULL;
	const char * filename = NULL;

	if (NULL == config)
	{
		filename = RTSP_DEF_CFG;
	}
	else
	{
		filename = config;
	}

    // read config file

	fp = fopen(filename, "r");
	if (NULL == fp)
	{
		goto FAILED;
	}

	fseek(fp, 0, SEEK_END);

	len = ftell(fp);
	if (len <= 0)
	{
		goto FAILED;
	}
	fseek(fp, 0, SEEK_SET);

	xml_buff = (char *) malloc(len + 1);
	if (NULL == xml_buff)
	{
		goto FAILED;
	}

	rlen = fread(xml_buff, 1, len, fp);
	if (rlen > 0)
	{
	    xml_buff[rlen] = '\0';
	    ret = rtsp_parse_config(xml_buff, rlen);
	}
	else
	{
	    log_print(LOG_ERR, "%s, rlen = %d, len=%d\r\n", __FUNCTION__, rlen, len);
	}

FAILED:

	if (fp)
	{
		fclose(fp);
	}

	if (xml_buff)
	{
		free(xml_buff);
	}

	return ret;
}

std::vector<int> rtsp_get_proxy_suffix_list()
{
	std::vector<int> ret;

	RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
	while( p_proxy ){
		ret.push_back(atoi(p_proxy->cfg.suffix));
		p_proxy = p_proxy->next;
	}

	return ret;
}


/***********************************************************/

BOOL rtsp_url_match(const char * rtspurl, const char * cfgurl)
{
	if (cfgurl == NULL || cfgurl[0] == '\0')
	{
		return TRUE;
	}

	// match the externsion name
	if (cfgurl[0] == '*' && cfgurl[1] == '.')
	{
		int len = strlen(cfgurl) - 1;
		int rtspurl_len = strlen(rtspurl);

		if (rtspurl_len > len)
		{
			int i;

			for (i = 0; i < len; i++)
			{
				if (rtspurl[rtspurl_len - len + i] != cfgurl[i + 1])
				{
					break;
				}
			}

			if (i == len)
			{
				return TRUE;
			}
		}
	}
	else if (strstr(rtspurl, cfgurl))
	{
		return TRUE;
	}

	return FALSE;
}

BOOL rtsp_cfg_get_video_info(const char * url, int * codec, int * width, int * height, int * framerate, int * bitrate)
{
	RTSP_OUTPUT * p_output = g_rtsp_cfg.output;
	while (p_output)
	{
		if (rtsp_url_match(url, p_output->url))
		{
			if (codec)
			{
				*codec = p_output->v_info.codec;
			}

			if (width)
			{
				*width = p_output->v_info.width;
			}

			if (height)
			{
				*height = p_output->v_info.height;
			}

			if (framerate)
			{
				*framerate = p_output->v_info.framerate;
			}

			if (bitrate)
			{
			    *bitrate = p_output->v_info.bitrate;
			}

			return TRUE;
		}

		p_output = p_output->next;
	}

	return FALSE;
}

BOOL rtsp_cfg_get_audio_info(const char * url, int * codec, int * samplerate, int * channels, int * bitrate)
{
	RTSP_OUTPUT * p_output = g_rtsp_cfg.output;
	while (p_output)
	{
		if (rtsp_url_match(url, p_output->url))
		{
			if (codec)
			{
				*codec = p_output->a_info.codec;
			}

			if (samplerate)
			{
				*samplerate = p_output->a_info.samplerate;
			}

			if (channels)
			{
				*channels = p_output->a_info.channels;
			}

			if (bitrate)
			{
			    *bitrate = p_output->a_info.bitrate;
			}

			return TRUE;
		}

		p_output = p_output->next;
	}

	return FALSE;
}

#ifdef RTSP_BACKCHANNEL
BOOL rtsp_cfg_get_backchannel_info(int * codec, int * samplerate, int * channels)
{
    if (codec)
	{
		*codec = g_rtsp_cfg.backchannel.codec;
	}

	if (samplerate)
	{
		*samplerate = g_rtsp_cfg.backchannel.samplerate;
	}

	if (channels)
	{
		*channels = g_rtsp_cfg.backchannel.channels;
	}

	return TRUE;
}
#endif



