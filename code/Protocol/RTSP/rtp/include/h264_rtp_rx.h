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

#ifndef	H264_RTP_RX_H
#define	H264_RTP_RX_H

#include "h264.h"
#include "rtp_rx.h"


typedef struct h264_rtp_rx_info
{
    RTPRXI      rtprxi;

	uint8     * p_buf_org;				// Allocated buffer
	uint8     * p_buf;					// = p_buf_org + 32
	int         buf_len;				// Buffer length -32 
	int         d_offset;				// Data offset 

	VRTPRXCBF   pkt_func;				// callback function
    void      * user_data;              // user data

    H264ParamSets   param_sets;         // h264 sps pps parameter sets
} H264RXI;

#ifdef __cplusplus
extern "C" {
#endif

BOOL h264_data_rx(H264RXI * p_rxi, uint8 * p_data, int len);
BOOL h264_rtp_rx(H264RXI * p_rxi, uint8 * p_data, int len);
BOOL h264_rxi_init(H264RXI * p_rxi, VRTPRXCBF cbf, void * p_userdata);
void h264_rxi_deinit(H264RXI * p_rxi);
void h264_save_sps(H264RXI* p_rxi, uint8* p_data, int len);
void h264_save_pps(H264RXI* p_rxi, uint8* p_data, int len);

#ifdef __cplusplus
}
#endif

#endif // H264_RTP_RX_H




