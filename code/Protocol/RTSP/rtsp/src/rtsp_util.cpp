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
#include "rtsp_util.h"


/***********************************************************************/
#define UPD_BASE_PORT   30000

static uint16 g_udp_bport = UPD_BASE_PORT;
static void * g_udp_port_mutex = sys_os_create_mutex();



/***********************************************************************/

uint16 rtsp_get_udp_port()
{
    uint16 port;

    sys_os_mutex_enter(g_udp_port_mutex);

    port = g_udp_bport;
    g_udp_bport += 2;
    if (g_udp_bport > 65534)
    {
        g_udp_bport = UPD_BASE_PORT;
    }

    sys_os_mutex_leave(g_udp_port_mutex);

    return port;
}

uint32 rtsp_get_timestamp(int frequency)
{
#if 0
	struct timeval tv;

	gettimeofday(&tv, 0);

	// Begin by converting from "struct timeval" units to RTP timestamp units:
	uint32 increment = (frequency*tv.tv_sec);
	increment += (uint32)(frequency*(tv.tv_usec/1000000.0) + 0.5); // note: rounding

	return increment;
#else
	struct timespec tv;
	clock_gettime( CLOCK_REALTIME, &tv );
	uint32 increment = ( frequency * tv.tv_sec );
	increment += (uint32)( frequency * ( tv.tv_nsec / 1000000000.0 ) + 0.5 ); // note: rounding
	return increment;
#endif
}

char * rtsp_get_utc_time()
{
	static char buff[100];

	time_t t = time(NULL);
	struct tm *ptr = gmtime(&t);

	strftime(buff, sizeof(buff)-1, "%a, %b %d %Y %H:%M:%S GMT", ptr);

	return buff;
}

int rtsp_pkt_find_end(char * p_buf)
{
	int end_off = 0;
	int rtsp_pkt_finish = 0;

	while (p_buf[end_off] != '\0')
	{
		if ((p_buf[end_off+0] == '\r' && p_buf[end_off+1] == '\n') &&
			(p_buf[end_off+2] == '\r' && p_buf[end_off+3] == '\n'))
		{
			rtsp_pkt_finish = 1;
			break;
		}

		end_off++;
	}

	if (rtsp_pkt_finish)
	{
		return(end_off + 4);
	}

	return 0;
}

int rtsp_pkt_find_end_len( const char* buf, int len )
{
	if( len < 4 )
		return 0;

	for( int i = 0; i <= len - 4; ++i )
	{
		if( buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n' )
			return i + 4; // 헤더끝 인덱스+4
	}
	return 0; // 더 필요
}

time_t rtsp_timegm(struct tm *T)
{
	time_t t, g, z;
	struct tm tm;

	t = mktime(T);
	if (t == (time_t)-1)
	{
		return (time_t)-1;
	}

	tm = *gmtime(&t);

	tm.tm_isdst = 0;
	g = mktime(&tm);
	if (g == (time_t)-1)
	{
		return (time_t)-1;
	}

	z = g - t;
	return t - z;
}

BOOL rtsp_parse_xsd_datetime(const char * s, time_t * p)
{
	if (s)
	{
		char zone[32];
		struct tm T;
		const char *t;

		*zone = '\0';
		memset(&T, 0, sizeof(T));

		if (strchr(s, '-'))
		{
			t = "%d-%d-%dT%d:%d:%d%31s";
		}
		else if (strchr(s, ':'))
		{
			t = "%4d%2d%2dT%d:%d:%d%31s";
		}
		else /* parse non-XSD-standard alternative ISO 8601 format */
		{
			t = "%4d%2d%2dT%2d%2d%2d%31s";
		}

		if (sscanf(s, t, &T.tm_year, &T.tm_mon, &T.tm_mday, &T.tm_hour, &T.tm_min, &T.tm_sec, zone) < 6)
		{
			return FALSE;
		}

		if (T.tm_year == 1)
		{
			T.tm_year = 70;
		}
		else
		{
			T.tm_year -= 1900;
		}

		T.tm_mon--;

		if (*zone == '.')
		{
			for (s = zone + 1; *s; s++)
			{
				if (*s < '0' || *s > '9')
				{
					break;
				}
			}
		}
    	else
    	{
      		s = zone;
      	}

		if (*s)
		{
			if (*s == '+' || *s == '-')
			{
				int h = 0, m = 0;
				if (s[3] == ':')
				{
					/* +hh:mm */
					sscanf(s, "%d:%d", &h, &m);
					if (h < 0)
						m = -m;
				}
				else /* +hhmm */
				{
					m = (int)strtol(s, NULL, 10);
					h = m / 100;
					m = m % 100;
				}

				T.tm_min -= m;
				T.tm_hour -= h;
				/* put hour and min in range */
				T.tm_hour += T.tm_min / 60;
				T.tm_min %= 60;

				if (T.tm_min < 0)
				{
					T.tm_min += 60;
					T.tm_hour--;
				}

				T.tm_mday += T.tm_hour / 24;
				T.tm_hour %= 24;

				if (T.tm_hour < 0)
				{
					T.tm_hour += 24;
					T.tm_mday--;
				}
				/* note: day of the month may be out of range, timegm() handles it */
			}

			*p = rtsp_timegm(&T);
		}
		else /* no UTC or timezone, so assume we got a localtime */
		{
			T.tm_isdst = -1;
			*p = mktime(&T);
		}
	}

	return TRUE;
}

int64 rtsp_gettime()
{
#if __LINUX_OS__
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64)tv.tv_sec * 1000000 + tv.tv_usec;
#elif __WINDOWS_OS__
    FILETIME ft;
    int64 t;
    GetSystemTimeAsFileTime(&ft);
    t = (int64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t / 10 - 11644473600000000; /* Jan 1, 1601 */
#endif

    return -1;
}

uint64 rtsp_ntp_time()
{
    return (rtsp_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}



