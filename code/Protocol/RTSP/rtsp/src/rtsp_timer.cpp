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
#include "rtsp_timer.h"
#include "rtsp_rsua.h"
#include "rtsp_srv.h"
#include "rtsp_stream.h"
#include "MgenLogger.h"


/***************************************************************************************/
extern RTSP_CLASS	hrtsp;

/***************************************************************************************
 timer message handler
****************************************************************************************/
void rtsp_timer()
{
	uint32 i;
	time_t cur_time = time(NULL);
	RSUA * p_sua = NULL;

	for (i = 0; i < MAX_NUM_RUA; i++)
	{
		p_sua = (RSUA *)rua_get_by_index(i);
		if (p_sua == NULL)
		{
			continue;
		}

		if (!p_sua->used_flag)
		{
			continue;
		}

		if (cur_time - p_sua->lats_rx_time > hrtsp.session_timeout)
		{
			// the session timeout, close it

			log_print(H_LOG_INFO, "%s, session timeout\r\n", __FUNCTION__);
			MLOG_WARN( "%s() => session_timeout ( %d s ), close session", __func__, hrtsp.session_timeout );
			rtsp_close_rua(p_sua);
		}

#ifdef RTSP_REPLAY
        // Playback arrival end time
		if (p_sua->replay && p_sua->play_range && p_sua->play_range_end != 0 &&
		    p_sua->play_range_type == 1 && cur_time >= p_sua->play_range_end - 1)
		{
		    rtsp_pause_stream_tx(p_sua);
		}
#endif
	}
}

#if	__WINDOWS_OS__

#ifdef _WIN64
void CALLBACK rtsp_win_timer(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
#else
void CALLBACK rtsp_win_timer(UINT uID, UINT uMsg, DWORD dwUser, DWORD dw1, DWORD dw2)
#endif
{
	if (hrtsp.sys_init_flag)
	{
		RIMSG stm;
		memset(&stm, 0, sizeof(RIMSG));

		stm.msg_src = RTSP_TIMER_SRC;

		hqBufPut(hrtsp.msg_queue, (char *)&stm);
	}
}

void rtsp_timer_init()
{
	hrtsp.timer_id = timeSetEvent(1000, 0, rtsp_win_timer, 0, TIME_PERIODIC);
}

void rtsp_timer_deinit()
{
	timeKillEvent(hrtsp.timer_id);
}

#else

void * rtsp_timer_task(void * argv)
{
	struct timeval tv;

	while (hrtsp.sys_timer_run == 1)
	{
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		select(1, NULL, NULL, NULL, &tv);

		RIMSG stm;
		memset(&stm, 0, sizeof(RIMSG));

		stm.msg_src = RTSP_TIMER_SRC;

		hqBufPut(hrtsp.msg_queue, (char *)&stm);
	}

	hrtsp.timer_id = 0;

	log_print(LOG_DBG, "rtsp timer task exit\r\n");

	return NULL;
}

void rtsp_timer_init()
{
	hrtsp.sys_timer_run = 1;

	pthread_t thrd = sys_os_create_thread((void *)rtsp_timer_task, NULL);
	if (thrd == 0)
	{
		log_print(LOG_ERR, "%s, rtsp_timer_task failed\r\n", __FUNCTION__);
		return;;
	}

	hrtsp.timer_id = thrd;

	log_print(LOG_DBG, "%s, create rtsp timer thread sucessful\r\n", __FUNCTION__);
}

void rtsp_timer_deinit()
{
	hrtsp.sys_timer_run = 0;

	while (hrtsp.timer_id != 0)
	{
		usleep(10*1000);
	}
}

#endif



