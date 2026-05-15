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

#ifndef MEDIA_UTIL_H
#define MEDIA_UTIL_H

#include "sys_inc.h"

#ifdef RTSP_FILE
extern "C" 
{
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/avstring.h>
#include <libavutil/base64.h>
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint32  remove_emulation_bytes(uint8* to, uint32 toMaxSize, uint8* from, uint32 fromSize);
uint8 * avc_find_startcode(uint8 *p, uint8 *end);
uint8 * avc_split_nalu(uint8 * e_buf, int e_len, int * s_len, int * d_len);

unsigned long gettickcount();
unsigned long gettickcount2();

#ifdef __cplusplus
}
#endif

#endif



