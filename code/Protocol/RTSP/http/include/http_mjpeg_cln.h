
#ifndef HTTP_MJPEG_CLN_H
#define HTTP_MJPEG_CLN_H

#include "http.h"
#include "http_parse.h"
#include "SafeThread.h"
#include <memory>


typedef int (*mjpeg_notify_cb)(int, void *);
typedef int (*mjpeg_video_cb)(uint8 *, int, void *);

#define MJPEG_EVE_STOPPED    20
#define MJPEG_EVE_CONNECTING 21
#define MJPEG_EVE_CONNFAIL   22
#define MJPEG_EVE_CONNSUCC   23
#define MJPEG_EVE_NOSIGNAL   24
#define MJPEG_EVE_RESUME     25
#define MJPEG_EVE_AUTHFAILED 26
#define MJPEG_EVE_NODATA   	 27

#define MJPEG_RX_ERR         -1
#define MJPEG_PARSE_ERR      -2
#define MJPEG_AUTH_ERR       -3
#define MJPEG_MALLOC_ERR     -4
#define MJPEG_MORE_DATA      0
#define MJPEG_RX_SUCC        1
#define MJPEG_NEED_AUTH      2


class CHttpMjpeg
{
public:
    CHttpMjpeg(void);
    ~CHttpMjpeg(void);

public:
	BOOL    mjpeg_start(const char * url, char * user, char * pass);
	BOOL    mjpeg_stop();

    void    set_notify_cb(mjpeg_notify_cb notify, void * userdata);
	void    set_video_cb(mjpeg_video_cb cb);

    void    rx_thread();

private:
    void    copy_str_from_url(char* dest, char const* src, uint32 len);
    BOOL    parse_url(char const* url, char*& username, char*& password, char*& address, int& portNum, char const** urlSuffix, int& https);

    BOOL    mjpeg_req(HTTPREQ * p_req);
    BOOL    mjpeg_conn(HTTPREQ * p_req, int timeout);
    void    mjpeg_data_rx(uint8 * data, int len);
    int     mjpeg_parse_header(HTTPREQ * p_user);
    int     mjpeg_parse_header_ex(HTTPREQ * p_user);
    int     mjpeg_rx(HTTPREQ * p_user);

    void    send_notify(int event);

    void    regist_http_mjpeg_cln_rx_thread_functions( void );

private:
    MGEN::SafeThread m_rxThread;
    HTTPREQ          m_req;
    BOOL             m_running;
    BOOL             m_header;

    mjpeg_notify_cb  m_pNotify;
	void *           m_pUserdata;
	mjpeg_video_cb   m_pVideoCB;
	void *			 m_pMutex;
};

#endif // end of HTTP_MJPEG_CLN_H


