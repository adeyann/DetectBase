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

#ifndef RTP_RX_H
#define RTP_RX_H


#define RTP_MAX_VIDEO_BUFF      (1024*1024)
#define RTP_MAX_AUDIO_BUFF      (8*1024)


typedef int (*VRTPRXCBF)(uint8 * p_data, int len, uint32 ts, uint32 seq, void * p_userdata);


typedef struct
{
    uint32      rxf_marker  : 1;        // Marker bit
    uint32      rxf_loss    : 1;        // The loss of sequence numbers has occurred in the middle
    uint32      res1        : 14;
    
	uint32	    prev_seq    : 16;	    // Previous sequence number
    
	uint32	    ssrc;                   // Synchronization source
	uint32	    prev_ts;				// Timestamp for the previous package 
	
    uint8     * p_data;
    int         len;
} RTPRXI;

#ifdef __cplusplus
extern "C" {
#endif

int rtp_data_rx(RTPRXI * p_rxi, uint8 * p_data, int len);

#ifdef __cplusplus
}
#endif


#endif


