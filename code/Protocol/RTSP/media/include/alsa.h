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

#ifndef ALSA_H
#define ALSA_H

#include "sys_inc.h"
#include <alsa/asoundlib.h>

/***************************************************************************************/

#define ALSA_BUFFER_SIZE_MAX 131072

/***************************************************************************************/

typedef struct
{
    char        devname[200];   // 
    snd_pcm_t * handler;        // 
    int         period_size;    // preferred size for reads and writes, in frames
    int         framesize;      // bytes per sample * channels
} ALSACTX;


#ifdef __cplusplus
extern "C" {
#endif

BOOL alsa_get_device_name(int index, char * filename, int len);
BOOL alsa_open_device(ALSACTX * p_alsa, snd_pcm_stream_t mode, uint32 * sample_rate, int channels);
int  alsa_xrun_recover(snd_pcm_t * handler, int err);


#ifdef __cplusplus
}
#endif


#endif



