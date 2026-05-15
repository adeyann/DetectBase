#include "sys_inc.h"
#include "http.h"
#include "http_parse.h"
#include "http_cln.h"
#include "http_mjpeg_cln.h"

void* mjpeg_rx_thread(void * argv) {
	CHttpMjpeg* p_mjpeg = (CHttpMjpeg*)argv;

	p_mjpeg->rx_thread();

	return (void*)NULL;
}

CHttpMjpeg::CHttpMjpeg(void)
	: m_running(FALSE)
	, m_header(FALSE)
	, m_pNotify(NULL)
	, m_pUserdata(NULL)
	, m_pVideoCB(NULL)
	, m_pMutex(NULL)
{
    memset(&m_req, 0, sizeof(m_req));

    m_pMutex = sys_os_create_mutex();

	this->regist_http_mjpeg_cln_rx_thread_functions();
}

CHttpMjpeg::~CHttpMjpeg(void)
{
    mjpeg_stop();

    if (m_pMutex) {
		sys_os_destroy_sig_mutex(m_pMutex);
		m_pMutex = NULL;
	}
}

void CHttpMjpeg::regist_http_mjpeg_cln_rx_thread_functions( void )
{
	this->m_rxThread.SetThreadFunctions(
		std::bind( &CHttpMjpeg::rx_thread, this )
	);
}

BOOL CHttpMjpeg::mjpeg_start(const char * url, char * user, char * pass)
{
    int port		= 80;
    int https		= 0;
    char* username	= NULL;
	char* password	= NULL;
	char* address	= NULL;
	char const* suffix = NULL;

	if (!parse_url(url, username, password, address, port, &suffix, https)) {
		return FALSE;
	}

	if (username) {
	    strcpy(m_req.user, username);
	    strcpy(m_req.auth_info.auth_name, username);
		delete[] username;
	}
	else if (user) {
	    strcpy(m_req.user, user);
	    strcpy(m_req.auth_info.auth_name, user);
	}

	if (password) {
	    strcpy(m_req.pass, password);
		strcpy(m_req.auth_info.auth_pwd, password);
		delete[] password;
	}
	else if (pass) {
	    strcpy(m_req.pass, pass);
	    strcpy(m_req.auth_info.auth_pwd, pass);
	}

    if (address) {
	    strncpy(m_req.host, address, sizeof(m_req.host) - 1);
	    delete[] address;
    }

    if (suffix) {
		strcpy(m_req.url, suffix);
		strcpy(m_req.auth_info.auth_uri, suffix);
	}

	m_req.port  = port;
	m_req.https = https;

	m_rxThread.Start();

    return TRUE;
}

BOOL CHttpMjpeg::mjpeg_stop()
{
    sys_os_mutex_enter(m_pMutex);
	m_pVideoCB  = NULL;
	m_pNotify   = NULL;
	m_pUserdata = NULL;
	sys_os_mutex_leave(m_pMutex);

	m_rxThread.Stop();

    http_cln_free_req(&m_req);

    m_header = 0;
    memset(&m_req, 0, sizeof(m_req));

    return TRUE;
}

void CHttpMjpeg::copy_str_from_url(char* dest, char const* src, uint32 len)
{
	// Normally, we just copy from the source to the destination.  However, if the source contains
	// %-encoded characters, then we decode them while doing the copy:
	while (len > 0)  {
		int nBefore = 0;
		int nAfter  = 0;

		if (*src == '%' && len >= 3 && sscanf(src+1, "%n%2hhx%n", &nBefore, dest, &nAfter) == 1) {
			uint32 codeSize = nAfter - nBefore; // should be 1 or 2

			++dest;
			src += (1 + codeSize);
			len -= (1 + codeSize);
		}
		else {
			*dest++ = *src++;
			--len;
		}
	}
	*dest = '\0';
}

BOOL CHttpMjpeg::parse_url(char const* url, char*& username, char*& password, char*& address, int& portNum, char const ** urlSuffix, int& https)
{
	do {
		uint32 prefixLength = 7;
		if (strnicmp(url, "http://", 7) == 0) {
			prefixLength = 7;
			https = 0;
		}
		else if (strnicmp(url, "https://", 7) == 0) {
		    prefixLength = 8;
		    https = 1;
		}
		else {
		    log_print(LOG_ERR, "%s, invalid URL %s\r\n", __FUNCTION__, url);
		    return FALSE;
		}

		uint32 const parseBufferSize		= 100;
		char parseBuffer[parseBufferSize]	= {'\0', };
		char const* from					= &url[prefixLength];

		// Check whether "<username>[:<password>]@" occurs next.
		// We do this by checking whether '@' appears before the end of the URL, or before the first '/'.
		username = password = address = NULL; // default return values
		char const* colonPasswordStart = NULL;
		char const* p;

		for (p = from; *p != '\0' && *p != '/'; ++p) {
			if (*p == ':' && colonPasswordStart == NULL) {
				colonPasswordStart = p;
			}
			else if (*p == '@') {
				// We found <username> (and perhaps <password>).  Copy them into newly-allocated result strings:
				if (colonPasswordStart == NULL)
					colonPasswordStart = p;

				char const* usernameStart = from;
				uint32 usernameLen = (uint32)(colonPasswordStart - usernameStart);
				username = new char[usernameLen + 1] ; // allow for the trailing '\0'
				copy_str_from_url(username, usernameStart, usernameLen);

				char const* passwordStart = colonPasswordStart;
				if (passwordStart < p) {
				    ++passwordStart; // skip over the ':'
				}

				uint32 passwordLen = (uint32)(p - passwordStart);
				password = new char[passwordLen + 1]; // allow for the trailing '\0'
				copy_str_from_url(password, passwordStart, passwordLen);

				from = p + 1; // skip over the '@'
				break;
			}
		}

		// Next, parse <server-address-or-name>
		char* to = &parseBuffer[0];
		uint32 i;

		for (i = 0; i < parseBufferSize; ++i) {
			if (*from == '\0' || *from == ':' || *from == '/')
			{
				// We've completed parsing the address
				*to = '\0';
				break;
			}
			*to++ = *from++;
		}

		if (i == parseBufferSize) {
			log_print(LOG_ERR, "%s, URL is too long\r\n", __FUNCTION__);
			break;
		}

		address = new char[strlen(parseBuffer)+1] ;
		strcpy(address, parseBuffer);

        if (https == 1)
		    portNum = 445;  // default value
		else
		    portNum = 80;   // default value

		char nextChar = *from;
		if (nextChar == ':') {
			int portNumInt;
			if (sscanf(++from, "%d", &portNumInt) != 1) {
				log_print(LOG_ERR, "%s, No port number follows ':'\r\n", __FUNCTION__);
				break;
			}

			if (portNumInt < 1 || portNumInt > 65535) {
				log_print(LOG_ERR, "%s, Bad port number\r\n", __FUNCTION__);
				break;
			}

			portNum = portNumInt;

			while (*from >= '0' && *from <= '9') {
				++from; // skip over port number
			}
		}

		// The remainder of the URL is the suffix:
		if (urlSuffix != NULL)
			*urlSuffix = from;

		return TRUE;
	} while (0);

  	return FALSE;
}

BOOL CHttpMjpeg::mjpeg_req(HTTPREQ * p_req)
{
    int  offset		  = 0;
	char bufs[4*1024] = {'\0',};

	offset += sprintf(bufs+offset, "GET %s HTTP/1.1\r\n", p_req->url);
	offset += sprintf(bufs+offset, "Host: %s:%d\r\n", p_req->host, p_req->port);
	offset += sprintf(bufs+offset, "Accept: */*\r\n");
	offset += sprintf(bufs+offset, "User-Agent: happytimesoft/1.0\r\n");
	offset += sprintf(bufs+offset, "Range: bytes=0-\r\n\r\n");

    if (p_req->need_auth) {
        offset += http_build_auth_msg(p_req, "GET", bufs+offset);
    }

	bufs[offset] = '\0';

    log_print(LOG_DBG, "TX >> %s\r\n", bufs);

	return http_cln_tx(p_req, bufs, offset);
}

BOOL CHttpMjpeg::mjpeg_conn(HTTPREQ * p_req, int timeout)
{
#ifdef HTTPS
	SSL_CTX* ctx = NULL;
	const SSL_METHOD* meth = NULL;

	if (p_req->https) {
		SSLeay_add_ssl_algorithms();
		meth = SSLv23_client_method();
		SSL_load_error_strings();
		ctx  = SSL_CTX_new(meth);
		if (NULL == ctx) {
			log_print(LOG_ERR, "%s, SSL_CTX_new failed!\r\n", __FUNCTION__);
			return FALSE;
		}
	}
#else
	if (p_req->https) {
		log_print(LOG_ERR, "%s, the server require ssl connection, unsupport!\r\n", __FUNCTION__);
		return FALSE;
	}
#endif

	p_req->cfd = tcp_connect_timeout(inet_addr(p_req->host), p_req->port, timeout);
	if (p_req->cfd <= 0) {
	    log_print(LOG_ERR, "%s, tcp_connect_timeout\r\n", __FUNCTION__);
		return FALSE;
    }

#ifdef HTTPS
	if (p_req->https) {
		p_req->ssl = SSL_new(ctx);
		if (NULL == p_req->ssl) {
			log_print(LOG_ERR, "%s, SSL_new failed!\r\n", __FUNCTION__);
			return FALSE;
		}

		SSL_set_fd(p_req->ssl, (int)p_req->cfd);

		if (SSL_connect(p_req->ssl) == -1) {
			log_print(LOG_ERR, "%s, SSL_connect failed!\r\n", __FUNCTION__);
			return FALSE;
		}
	}
#endif

	return TRUE;
}

int CHttpMjpeg::mjpeg_parse_header(HTTPREQ * p_user)
{
    if (http_is_http_msg(p_user->p_rbuf) == FALSE) {
		return -1;
	}

	HTTPMSG * rx_msg = NULL;

	int parse_len;
	int http_pkt_len;

	http_pkt_len = http_pkt_find_end(p_user->p_rbuf);
	if (http_pkt_len == 0) {
		return 0;
	}

	rx_msg = http_get_msg_buf();
	if (rx_msg == NULL) {
		log_print(LOG_ERR, "%s, get_msg_buf ret null!!!\r\n", __FUNCTION__);
		return -1;
	}

	memcpy(rx_msg->msg_buf, p_user->p_rbuf, http_pkt_len);
	rx_msg->msg_buf[http_pkt_len] = '\0';

	log_print(LOG_DBG, "RX from %s << %s\r\n", p_user->host, rx_msg->msg_buf);

	parse_len = http_msg_parse_part1(rx_msg->msg_buf, http_pkt_len, rx_msg);
	if (parse_len != http_pkt_len) {
		log_print(LOG_ERR, "%s, http_msg_parse_part1=%d, http_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, http_pkt_len);

		http_free_msg(rx_msg);
		return -1;
	}

	p_user->rx_msg = rx_msg;

    p_user->rcv_dlen = p_user->rcv_dlen-parse_len;
    if (p_user->rcv_dlen > 0) {
	    memcpy(p_user->p_rbuf, p_user->p_rbuf+parse_len, p_user->rcv_dlen);
	}

	return 1;
}

int CHttpMjpeg::mjpeg_parse_header_ex(HTTPREQ * p_user)
{
	HTTPMSG * rx_msg = NULL;

    int   offset   = 0;
	int   line_len = 0;
	int   http_pkt_len;
    BOOL  bHaveNextLine;
	char* p_buf;

    while (*(p_user->p_rbuf+offset) == '\r' || *(p_user->p_rbuf+offset) == '\n') {
        offset++;
    }

	http_pkt_len = http_pkt_find_end(p_user->p_rbuf+offset);
	if (http_pkt_len == 0)  {
		return 0;
	}
	p_user->hdr_len = http_pkt_len+offset;

	rx_msg = http_get_msg_buf();
	if (rx_msg == NULL) {
		log_print(LOG_ERR, "%s, get_msg_buf ret null!!!\r\n", __FUNCTION__);
		return -1;
	}

	memcpy(rx_msg->msg_buf, p_user->p_rbuf+offset, http_pkt_len);
	rx_msg->msg_buf[http_pkt_len] = '\0';

	log_print(LOG_DBG, "RX from %s << %s\r\n", p_user->host, rx_msg->msg_buf);

    p_buf = rx_msg->msg_buf;

READ_LINE:

	if (GetSipLine(p_buf, http_pkt_len, &line_len, &bHaveNextLine) == FALSE) {
		return -1;
	}

    if (line_len < 2) {
        return -1;
    }

	offset = 0;
	while (*(p_buf+offset) == '-') {
	    offset++;
	}

	if (strncmp(p_buf+offset, p_user->boundary, strlen(p_user->boundary))) {
	    p_buf += line_len;
		goto READ_LINE;
	}

	p_buf += line_len;
	rx_msg->hdr_len = http_line_parse(p_buf, http_pkt_len-line_len, ':', &(rx_msg->hdr_ctx));
	if (rx_msg->hdr_len <= 0) {
		return -1;
	}

	http_ctt_parse(rx_msg);

	p_user->rx_msg = rx_msg;
	p_user->ctt_len = rx_msg->ctt_len;

	return 1;
}

void CHttpMjpeg::mjpeg_data_rx(uint8 * data, int len)
{
#if 1
    static int cnt = 1;
    char name[32];

    sprintf(name, "%d.jpg", cnt++);

    FILE * fp = fopen(name, "wb");
    if (fp) {
        fwrite(data, len, 1, fp);
    }
#endif

    sys_os_mutex_enter(m_pMutex);
	if (m_pVideoCB) {
		m_pVideoCB(data, len, m_pUserdata);
	}
	sys_os_mutex_leave(m_pMutex);
}

int CHttpMjpeg::mjpeg_rx(HTTPREQ * p_user)
{
#ifdef HTTPS
	int pending;
#endif

    int ret;
	int rlen;

	if (p_user->p_rbuf == NULL) {
		p_user->p_rbuf = p_user->rcv_buf;
		p_user->mlen   = sizeof(p_user->rcv_buf)-1;
		p_user->rcv_dlen = 0;
		p_user->ctt_len  = 0;
		p_user->hdr_len  = 0;
	}

#ifdef HTTPS

READ:

	if (p_user->https)
		rlen = SSL_read(p_user->ssl, p_user->p_rbuf+p_user->rcv_dlen, p_user->mlen-p_user->rcv_dlen);
	else
		rlen = recv(p_user->cfd, p_user->p_rbuf+p_user->rcv_dlen, p_user->mlen-p_user->rcv_dlen, 0);
#else
	rlen = recv(p_user->cfd, p_user->p_rbuf+p_user->rcv_dlen, p_user->mlen-p_user->rcv_dlen, 0);
#endif
	if (rlen <= 0) {
		log_print(H_LOG_INFO, "%s, recv return = %d, dlen[%d], mlen[%d]\r\n", __FUNCTION__, rlen, p_user->rcv_dlen, p_user->mlen);

		closesocket(p_user->cfd);
		p_user->cfd = 0;
		return MJPEG_RX_ERR;
	}

	p_user->rcv_dlen += rlen;
	p_user->p_rbuf[p_user->rcv_dlen] = '\0';

rx_analyse_point:
	if (p_user->rcv_dlen < 16) {
		return MJPEG_MORE_DATA;
	}

    if (!m_header) {
        ret = mjpeg_parse_header(p_user);
        if (ret > 0) {
            if (p_user->rx_msg->msg_sub_type == 401 && p_user->need_auth == FALSE) {
    			if (http_get_digest_info(p_user->rx_msg, &p_user->auth_info)) {
    				http_cln_free_req(p_user);
    				p_user->auth_mode = 1;
    			}
    			else {
    				http_cln_free_req(p_user);
    				p_user->auth_mode = 0;
    			}
    			p_user->need_auth = TRUE;

    			return MJPEG_NEED_AUTH;
    		}
    		else if (p_user->rx_msg->msg_sub_type == 401 && p_user->need_auth == TRUE) {
    		    return MJPEG_AUTH_ERR;
    		}

            if (p_user->rx_msg->ctt_type != CTT_MULTIPART) {
                http_free_msg(p_user->rx_msg);
                p_user->rx_msg = NULL;

                return MJPEG_PARSE_ERR;
            }

			m_header = 1;
            strcpy(p_user->boundary, p_user->rx_msg->boundary);

            http_free_msg(p_user->rx_msg);
            p_user->rx_msg = NULL;

            send_notify(MJPEG_EVE_CONNSUCC);
        }
        else if (ret < 0) {
            return MJPEG_PARSE_ERR;
        }
    }

    if (!m_header) {
#ifdef HTTPS
		if (p_user->https) {
			pending = SSL_pending(p_user->ssl);
			if (pending > 0)
        		goto READ;
		}
#endif
        return MJPEG_MORE_DATA;
    }

    if (p_user->rcv_dlen < 16)
		return MJPEG_MORE_DATA;

	if (p_user->hdr_len == 0) {
    	ret = mjpeg_parse_header_ex(p_user);
    	if (ret < 0) {
    	    return MJPEG_PARSE_ERR;
    	}
    	else if (ret == 0) {
#ifdef HTTPS
    		if (p_user->https) {
    			pending = SSL_pending(p_user->ssl);
    			if (pending > 0)
            		goto READ;
    		}
#endif
            return MJPEG_MORE_DATA;
    	}
    	else {
    	    if (p_user->rx_msg->ctt_type != CTT_JPG) {
    	        http_free_msg(p_user->rx_msg);
                p_user->rx_msg = NULL;

                return MJPEG_PARSE_ERR;
			}

    	    http_free_msg(p_user->rx_msg);
            p_user->rx_msg = NULL;
    	}
	}

	if ((p_user->ctt_len + p_user->hdr_len) > p_user->mlen) {
		if (p_user->dyn_recv_buf) {
			free(p_user->dyn_recv_buf);
			p_user->dyn_recv_buf = NULL;
		}

		p_user->dyn_recv_buf = (char *)malloc(p_user->ctt_len + p_user->hdr_len + 1);
		if (NULL == p_user->dyn_recv_buf) {
		    return MJPEG_MALLOC_ERR;
		}

		memcpy(p_user->dyn_recv_buf, p_user->rcv_buf, p_user->rcv_dlen);
		p_user->p_rbuf = p_user->dyn_recv_buf;
		p_user->mlen = p_user->ctt_len + p_user->hdr_len;

#ifdef HTTPS
		if (p_user->https) {
			pending = SSL_pending(p_user->ssl);
			if (pending > 0)
        		goto READ;
		}
#endif
		return MJPEG_MORE_DATA;
	}

	if (p_user->rcv_dlen >= (p_user->ctt_len + p_user->hdr_len)) {
        mjpeg_data_rx((uint8 *)p_user->p_rbuf+p_user->hdr_len, p_user->ctt_len);

		p_user->rcv_dlen -= p_user->hdr_len + p_user->ctt_len;

		if (p_user->dyn_recv_buf == NULL) {
			if (p_user->rcv_dlen > 0) {
				memmove(p_user->rcv_buf, p_user->rcv_buf+p_user->hdr_len + p_user->ctt_len, p_user->rcv_dlen);
				p_user->rcv_buf[p_user->rcv_dlen] = '\0';
			}

			p_user->p_rbuf  = p_user->rcv_buf;
			p_user->mlen    = sizeof(p_user->rcv_buf)-1;
			p_user->hdr_len = 0;
			p_user->ctt_len = 0;

			if (p_user->rcv_dlen > 16)
				goto rx_analyse_point;
		}
		else {
			free(p_user->dyn_recv_buf);
			p_user->dyn_recv_buf = NULL;
			p_user->hdr_len		 = 0;
			p_user->ctt_len		 = 0;
			p_user->p_rbuf		 = 0;
			p_user->rcv_dlen	 = 0;
		}
	}

	return MJPEG_RX_SUCC;
}

void CHttpMjpeg::rx_thread()
{
    int  ret;
    int  tm_count = 0;
    BOOL nodata_notify = FALSE;
	int  sret;
	fd_set fdr;
	struct timeval tv;

	// get loop flag
	auto& running = this->m_rxThread.GetRunningFlag();

    send_notify(MJPEG_EVE_CONNECTING);

RETRY:
	if (mjpeg_conn(&m_req, 15*1000) == FALSE) {
	    send_notify(MJPEG_EVE_CONNFAIL);
	    goto FAILED;
	}

	mjpeg_req(&m_req);

	while( running.load() == true ) {
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;

		FD_ZERO(&fdr);
		FD_SET(m_req.cfd, &fdr);

		sret = select((int)(m_req.cfd+1), &fdr, NULL, NULL, &tv);
		if (sret <= 0) {
		    tm_count++;
	        if (tm_count >= 100 && !nodata_notify)    // in 10s without data
	        {
	            nodata_notify = TRUE;
	            send_notify(MJPEG_EVE_NODATA);
	        }
			continue;
		}
		else {
		    if (nodata_notify) {
	            nodata_notify = FALSE;
	            send_notify(MJPEG_EVE_RESUME);
	        }
        	tm_count = 0;
		}

		if (FD_ISSET(m_req.cfd, &fdr)) {
		    ret = mjpeg_rx(&m_req);
			if (ret < 0) {
			    if (ret == MJPEG_AUTH_ERR) {
			        send_notify(MJPEG_EVE_AUTHFAILED);
			    }

			    log_print(LOG_ERR, "%s, mjpeg_rx failed. ret = %d\r\n", __FUNCTION__, ret);
				break;
			}
			else if (ret == MJPEG_NEED_AUTH) {
			    goto RETRY;
			}
		}
	}

    send_notify(MJPEG_EVE_STOPPED);

FAILED:
    log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);
}

void CHttpMjpeg::set_notify_cb(mjpeg_notify_cb notify, void * userdata)
{
	sys_os_mutex_enter(m_pMutex);
	m_pNotify   = notify;
	m_pUserdata = userdata;
	sys_os_mutex_leave(m_pMutex);
}

void CHttpMjpeg::set_video_cb(mjpeg_video_cb cb)
{
	sys_os_mutex_enter(m_pMutex);
	m_pVideoCB = cb;
	sys_os_mutex_leave(m_pMutex);
}

void CHttpMjpeg::send_notify(int event)
{
	sys_os_mutex_enter(m_pMutex);
	if (m_pNotify)
		m_pNotify(event, m_pUserdata);
	sys_os_mutex_leave(m_pMutex);
}



