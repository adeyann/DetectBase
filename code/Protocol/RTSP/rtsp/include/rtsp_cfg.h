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

#ifndef RTSP_CFG_H
#define RTSP_CFG_H

#include "rtsp_comm_cfg.h"
#ifdef PROXY
#include "rtsp_proxy.h"
#endif
#ifdef PUSHER
#include "rtsp_pusher.h"
#endif
#include "rtsp_srv.h"

#define MAX_USERS   10


typedef struct
{
	char 	Username[32];
	char 	Password[32];
} RTSP_USER;

typedef struct
{
    int     codec;                  // audio codec, refer media_format.h
    int     samplerate;             // sample rate
    int     channels;               // channels
} RTSP_BC_CFG;

typedef struct _RTSP_CFG
{
    int             serverip_nums;  //
    char            serverip[NET_IF_NUM][128];  // bind server ip addr
    int             serverport;     // bind server port
    uint32          loop_nums;      // file loop-play numbers

    BOOL            need_auth;      // auth flag
    int             crypt;          // data crypt flag
    BOOL            log_enable;     // log enable flag
    int             log_level;      // log level

    BOOL            multicast;      // enable multicast

#ifdef RTSP_METADATA
    BOOL            metadata;       // enable metadata stream
#endif

#ifdef RTSP_OVER_HTTP
    BOOL            rtsp_over_http;
    int             http_port;
#endif

    RTSP_USER       users[MAX_USERS];

    RTSP_OUTPUT   * output;         // the audio & video output configuration

#ifdef PROXY
    RTSP_PROXY    * proxy;          // proxy configurations
#endif

#ifdef PUSHER
    RTSP_PUSHER   * pusher;         // pusher configurations
#endif

#ifdef RTSP_BACKCHANNEL
    RTSP_BC_CFG     backchannel;    // back channel configuration
#endif
} RTSP_CFG;

#ifdef __cplusplus
extern "C" {
#endif

extern RTSP_CFG g_rtsp_cfg;

/**
 * read configuration file
 *
 * @param config the configuration file name
 * @return TRUE on success, FALSE on error
 */
BOOL         rtsp_read_config(const char * config);
const char * rtsp_get_user_pass(const char * username);
BOOL         rtsp_cfg_get_video_info(const char * url, int * codec, int * width, int * height, int * framerate, int * bitrate);
BOOL         rtsp_cfg_get_audio_info(const char * url, int * codec, int * samplerate, int * channels, int * bitrate);
BOOL         rtsp_cfg_get_backchannel_info(int * codec, int * samplerate, int * channels);
void         rtsp_free_outputs(RTSP_OUTPUT ** p_output);
void         rtsp_camera_list(std::vector<int>& cameraIds);

std::vector<int> rtsp_get_proxy_suffix_list();

#ifdef __cplusplus
}
#endif

#endif // RTSP_CFG_H


