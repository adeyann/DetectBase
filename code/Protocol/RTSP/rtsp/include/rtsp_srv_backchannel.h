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

#ifndef RTSP_SRV_BACKCHANNEL_H
#define RTSP_SRV_BACKCHANNEL_H

#include "rtsp_rsua.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL    rtsp_bc_init_audio(RSUA * p_rua);
void    rtsp_bc_uninit_audio(RSUA * p_rua);
BOOL    rtsp_bc_audio_rx(RSUA * p_rua, uint8 * lpData, int rlen, uint32 seq, uint32 ts);
void *  rtsp_bc_udp_rx_thread(void * argv);
void    rtsp_bc_tcp_data_rx(RSUA * p_rua, uint8 * lpData, int rlen);
void    rtsp_bc_udp_data_rx(RSUA * p_rua, uint8 * lpData, int rlen);

#ifdef __cplusplus
}
#endif

#endif



