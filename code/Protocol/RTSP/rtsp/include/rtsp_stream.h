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

#ifndef	__H_RTSP_STREAM_H__
#define	__H_RTSP_STREAM_H__

#ifdef __cplusplus
extern "C" {
#endif

int  rtsp_start_stream_tx(RSUA * p_rua);
int  rtsp_stop_stream_tx(RSUA * p_rua);
int  rtsp_terminate_all_stream_queue(RSUA * p_rua);
int  rtsp_pause_stream_tx(RSUA * p_rua);
int  rtsp_restart_stream_tx(RSUA * p_rua);


#ifdef __cplusplus
}
#endif

#endif // __H_RTSP_STREAM_H__


