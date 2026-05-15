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

#ifndef	RTP_TX_H
#define	RTP_TX_H

#include "rtsp_rsua.h"

typedef struct
{	
    int     limit_tx_rate;			    // limit the maxt tx speed per second
	int     limit_time_unit;		    // the limit time unit, microsecond
	int     max_in_time_unit;	        // the max tx byte in time unit
	int     tx_total;                   // cur time unit the total tx bytes		
    uint32  prev_time;
    
	void *  limit_lock;                 // lock
} RTP_TX_SHAPER;


#ifdef __cplusplus
extern "C" {
#endif

#ifdef TX_SHAPER
void rtp_init_tx_shaper();
#endif

/**
 * Build h264 video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_h264_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);

/**
 * Build h265 video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_h265_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);

/**
 * Build video rtp packet and send (not fragment)
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);

/**
 * Build jpeg video rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return the send data length, -1 on error
 */
int rtp_jpeg_video_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);

/**
 * Build audio rtp packet and send (not fragment)
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param len payload data length
 * @ts the packet timestamp
 * @return the rtp packet length, -1 on error
 */
int rtp_audio_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);

/**
 * Build AAC audio rtp packet and send
 *
 * @param p_rua rtsp user agent
 * @param p_data payload data
 * @param size payload data length
 * @ts the packet timestamp
 * @return 1 on success, -1 on error
 */
int rtp_aac_audio_tx(RSUA * p_rua, uint8 * p_data, int len, uint32 ts);


int rtp_metadata_tx(RSUA * p_rua, uint8 * p_data, int size, uint32 ts);

#ifdef __cplusplus
}
#endif

#endif	// RTP_TX_H



