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
#include "rtsp_parse.h"
#include "rtsp_rsua.h"
#include "rtsp_stream.h"
#include "rtp_tx.h"


/***********************************************************************/
void * rtsp_send_thread(void * argv)
{
	rtsp_media_send_thread(argv);
	return NULL;
}

int rtsp_terminate_all_stream_queue( RSUA* p_rua )
{
	if( p_rua )
		rtsp_terminate_queue( (void*)p_rua );
	return 0;
}

/***********************************************************************/

int rtsp_start_stream_tx(RSUA * p_rua)
{
	if (p_rua)
	{
		p_rua->rtp_tx = 1;

		p_rua->rtp_thread = sys_os_create_thread((void *)rtsp_send_thread, p_rua);
	}

	return 0;
}

int rtsp_stop_stream_tx(RSUA * p_rua)
{
    //printf("try to stop rtsp stream\n");
	if (p_rua)
	{
		p_rua->rtp_tx = 0;
		//p_rua->stopRtpTx = 1;
		while (p_rua->rtp_thread)
		{
            //printf("p_rua rtsp_thread still exists wait till it destructs");
			usleep(10*1000);
		}
	}
    //printf("stopped rtsp stream\n");

	return 0;
}

int rtsp_pause_stream_tx(RSUA * p_rua)
{
    p_rua->rtp_pause = 1;

#ifdef RTSP_FILE
    if (p_rua->media_info.media_file)
    {
        CFileDemux * p_Demux = p_rua->media_info.file_demuxer;
        if (p_Demux)
        {
            p_rua->media_info.curpos = p_Demux->getCurPos();
        }
    }
#endif

    return 0;
}

int rtsp_restart_stream_tx(RSUA * p_rua)
{
    p_rua->rtp_pause = 0;

    return 0;
}




