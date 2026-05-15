
#ifndef RTSP_HTTP_H
#define RTSP_HTTP_H

#include "http.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL rtsp_http_init();
void rtsp_http_deinit();
BOOL rtsp_http_process(HTTPCLN * p_user, HTTPMSG * rx_msg);
BOOL rtsp_http_msg_process(HTTPCLN * p_user, char * p_buff, int len);


#ifdef __cplusplus
}
#endif

#endif // end of RTSP_HTTP_H


