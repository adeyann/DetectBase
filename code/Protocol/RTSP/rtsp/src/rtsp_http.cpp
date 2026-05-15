
#include "sys_inc.h"
#include "http.h"
#include "http_parse.h"
#include "http_srv.h"
#include "rtsp_http.h"
#include "rtsp_srv.h"
#include "rtsp_cfg.h"
#include "rtsp_rsua.h"
#include "base64.h"

#ifdef RTSP_OVER_HTTP

/***********************************************************************/
HTTPSRV             hsrv;
extern RTSP_CLASS	hrtsp;
extern RTSP_CFG     g_rtsp_cfg;

/***********************************************************************/
BOOL rtsp_http_init()
{
    http_msg_buf_init(MAX_NUM_RUA * 2);

	if (g_rtsp_cfg.http_port <= 0 || g_rtsp_cfg.http_port > 65535)
	{
		g_rtsp_cfg.http_port = 80;
	}
    	
    http_srv_init(&hsrv, 0, g_rtsp_cfg.http_port, MAX_NUM_RUA);

    return TRUE;
}

void rtsp_http_deinit()
{
	http_srv_deinit(&hsrv);

	http_msg_buf_deinit();
}

int rtsp_http_rly(HTTPCLN * p_user, HTTPMSG * rx_msg, const char * p_xml, int len)
{
    int tlen;
	char * p_bufs;

	p_bufs = (char *)malloc(len + 1024);
	if (NULL == p_bufs)
	{
		return -1;
	}
	
	tlen = sprintf(p_bufs,	"HTTP/1.1 200 OK\r\n"
							"Server: hsoap/2.8\r\n"
							"Content-Type: %s\r\n"
							"Content-Length: %d\r\n\r\n",
							"application/x-rtsp-tunnelled", len);

	if (p_xml && len > 0)
	{
		memcpy(p_bufs+tlen, p_xml, len);
		tlen += len;
	}

	p_bufs[tlen] = '\0';
	log_print(LOG_DBG, "TX >> %s\r\n\r\n", p_bufs);

	tlen = send(p_user->cfd, p_bufs, tlen, 0);

	free(p_bufs);
	
	return tlen;
}

BOOL rtsp_http_process(HTTPCLN * p_user, HTTPMSG * rx_msg)
{
    char * p = http_get_headline(rx_msg, "x-sessioncookie");
    if (NULL == p)
    {
        return FALSE;
    }

    if (p_user->p_rua)
    {
        log_print(LOG_ERR, "%s, rua already exist\r\n", __FUNCTION__);
        return FALSE;
    }
    
    if (strstr(rx_msg->first_line.header, "GET"))
    {
        RSUA * p_rua = rua_get_idle_rua();
        if (NULL == p_rua)
        {
            log_print(LOG_ERR, "%s, rua_get_idle_rua failed\r\n", __FUNCTION__);
            return FALSE;
        }

        p_rua->rtp_tcp = 1;
        p_rua->rtsp_data = p_user;
        p_rua->lats_rx_time = time(NULL);
        strncpy(p_rua->sessioncookie, p, sizeof(p_rua->sessioncookie)-1);
        
        rua_set_online_rua(p_rua);
        
        p_user->p_rua = p_rua;

        rtsp_http_rly(p_user, rx_msg, NULL, 0);
    }
    else if (strstr(rx_msg->first_line.header, "POST"))
    {
        RSUA * p_rua = rua_get_by_sessioncookie(p);        
        if (p_rua)
        {
        	p_rua->lats_rx_time = time(NULL);
            p_rua->rtsp_cmd = p_user;
            p_user->p_rua = p_rua;
        }
    }

    return TRUE;
}

BOOL rtsp_http_msg_process(HTTPCLN * p_user, char * p_buff, int len)
{
    if (NULL == p_user->p_rua)
    {
        log_print(LOG_ERR, "%s, rua is null\r\n", __FUNCTION__);
        return FALSE;
    }

    int    bufflen;
    char * buff[2048] = {0,};
    RSUA * p_rua = (RSUA *) p_user->p_rua;  

    bufflen = base64_decode(p_buff, (uint8 *)buff, sizeof(buff));
    if (bufflen == -1)
    {
        log_print(LOG_ERR, "%s, base64_decode failed\r\n", __FUNCTION__);
        return FALSE;
    }

    memcpy(p_rua->rcv_buf+p_rua->rcv_dlen, buff, bufflen);
    p_rua->rcv_dlen += bufflen;
    
    if (rtsp_is_rtsp_msg(p_rua->rcv_buf))
	{
		int ret = rtsp_msg_parser(p_rua);
		if (RTSP_PARSE_FAIL == ret)
		{
		    log_print(LOG_ERR, "%s, rtsp_msg_parser failed\r\n", __FUNCTION__);
		    return FALSE;
		}
	}
    else
    {
    	p_rua->rcv_dlen = 0;
        log_print(LOG_ERR, "%s, rtsp_is_rtsp_msg failed\r\n", __FUNCTION__);
    }

	p_rua->lats_rx_time = time(NULL);
	
	return TRUE;
}

#endif // end of RTSP_OVER_HTTP



