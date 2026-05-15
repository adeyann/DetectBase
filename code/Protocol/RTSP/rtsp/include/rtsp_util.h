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

#ifndef RTSP_UTIL_H
#define RTSP_UTIL_H

#define NTP_OFFSET 			2208988800ULL
#define NTP_OFFSET_US 		(NTP_OFFSET * 1000000ULL)


#ifdef __cplusplus
extern "C" {
#endif

uint16  rtsp_get_udp_port();
uint32  rtsp_get_timestamp(int frequency);
char *  rtsp_get_utc_time();
int     rtsp_pkt_find_end(char * p_buf);
int     rtsp_pkt_find_end_len( const char* buf, int len );
BOOL 	rtsp_parse_xsd_datetime(const char * s, time_t * p);
int64	rtsp_gettime();
uint64 	rtsp_ntp_time();


#ifdef __cplusplus
}
#endif


#endif


