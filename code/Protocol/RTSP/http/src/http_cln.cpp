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
#include "http.h"
#include "http_parse.h"
#include "http_cln.h"
#include "rfc_md5.h"
#include "base64.h"

/***************************************************************************************/

#ifdef HTTPS
#if __WINDOWS_OS__
#include "openssl/applink.c"
#endif

#pragma comment(lib, "ssleay32.lib")
#endif

/***************************************************************************************/

#define MAX_CTT_LEN     (2*1024*1024)


/***************************************************************************************/

BOOL http_get_digest_info(HTTPMSG * rx_msg, HD_AUTH_INFO * auth_info)
{
	char word_buf[128];
	int	 next_offset;	
	HDRV * chap_id = NULL;
	
	chap_id = http_find_headline(rx_msg, "WWW-Authenticate");
	if (chap_id == NULL) {
		return FALSE;
	}

	auth_info->auth_response[0] = '\0';
	auth_info->auth_uri[0]		= '\0';

RETRY:
	word_buf[0] = '\0';	
	GetLineWord(chap_id->value_string, 0, (int)strlen(chap_id->value_string),
		word_buf, sizeof(word_buf), &next_offset, WORD_TYPE_STRING);

	if (strcasecmp(word_buf, "digest") != 0) {
		// There may be multiple "WWW-Authenticate" header line
		chap_id = http_find_headline_next(rx_msg, "WWW-Authenticate", chap_id);
		if (chap_id) {
			goto RETRY;
		}
		return FALSE;
	}
	
	word_buf[0] = '\0';
	if (GetNameValuePair(chap_id->value_string+next_offset,
		(int)strlen(chap_id->value_string)-next_offset, "realm", word_buf, sizeof(word_buf)) == FALSE) {	
		return FALSE;
	}
	strcpy(auth_info->auth_realm, word_buf);

	word_buf[0] = '\0';
	if (GetNameValuePair(chap_id->value_string+next_offset,
		(int)strlen(chap_id->value_string)-next_offset, "nonce", word_buf, sizeof(word_buf)) == FALSE) {	
		return FALSE;
	}	
	strcpy(auth_info->auth_nonce, word_buf);

	word_buf[0] = '\0';
	if (GetNameValuePair(chap_id->value_string+next_offset,
		(int)strlen(chap_id->value_string)-next_offset, "qop", word_buf, sizeof(word_buf))) {	
		strcpy(auth_info->auth_qop, word_buf);
	}	
	else {
		auth_info->auth_qop[0] = '\0';
	}
	
    word_buf[0] = '\0';
	if (GetNameValuePair(chap_id->value_string+next_offset,
		(int)strlen(chap_id->value_string)-next_offset, "opaque", word_buf, sizeof(word_buf))) {		
		strcpy(auth_info->auth_opaque, word_buf);
	}	
	else {
		auth_info->auth_opaque[0] = '\0';
	}
	
    sprintf(auth_info->auth_cnonce, "%08X%08X", rand(), rand());
	auth_info->auth_nc++;

	sprintf(auth_info->auth_ncstr, "%08X", auth_info->auth_nc);
	
	return TRUE;
}

BOOL http_calc_auth_digest(HD_AUTH_INFO * auth_info, const char * method)
{
    MD5_CTX Md5Ctx;
	HASH HA1;
	HASH HA2;
	HASH HA3;

	HASHHEX HA1Hex;
	HASHHEX HA2Hex;
	HASHHEX HA3Hex;	

	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_name, (int)strlen(auth_info->auth_name));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_realm, (int)strlen(auth_info->auth_realm));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_pwd, (int)strlen(auth_info->auth_pwd));
	MD5Final(HA1, &Md5Ctx);

	BinToHexStr(HA1, HA1Hex);

	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)method, (int)strlen(method));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_uri, (int)strlen(auth_info->auth_uri));
	MD5Final(HA2, &Md5Ctx);

	BinToHexStr(HA2, HA2Hex);

	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)HA1Hex, HASHHEXLEN);
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_nonce, (int)strlen(auth_info->auth_nonce));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);

	if (auth_info->auth_qop[0] != '\0') {
		MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_ncstr, (int)strlen(auth_info->auth_ncstr));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_cnonce, (int)strlen(auth_info->auth_cnonce));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)auth_info->auth_qop, (int)strlen(auth_info->auth_qop));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	};
	
	MD5Update(&Md5Ctx, (uint8 *)HA2Hex, HASHHEXLEN);
	MD5Final(HA3, &Md5Ctx);

	BinToHexStr(HA3, HA3Hex);

	strcpy(auth_info->auth_response, HA3Hex);
	
	return TRUE;
}

int http_build_auth_msg(HTTPREQ * p_user, const char * method, char * buff)
{
	int offset = 0;
	
	if (p_user->auth_mode == 1)			// digest auth
    {
        if (p_user->auth_info.auth_qop[0] != '\0') {
            strcpy(p_user->auth_info.auth_qop, "auth");
        }
        
        http_calc_auth_digest(&p_user->auth_info, method);

        offset += sprintf(buff+offset, 
						  "Authorization: Digest username=\"%s\",realm=\"%s\","
	    				  "nonce=\"%s\",uri=\"%s\",response=\"%s\"",
						  p_user->auth_info.auth_name, p_user->auth_info.auth_realm, p_user->auth_info.auth_nonce, 
						  p_user->auth_info.auth_uri, p_user->auth_info.auth_response);
    			
        if (p_user->auth_info.auth_qop[0] != '\0') {
            offset += sprintf(buff+offset, ",qop=\"auth\",cnonce=\"%s\",nc=%s",
    						  p_user->auth_info.auth_cnonce, p_user->auth_info.auth_ncstr);
        }

        if (p_user->auth_info.auth_opaque[0] != '\0') {
            offset += sprintf(buff+offset, ",opaque=\"%s\"", p_user->auth_info.auth_opaque);
        }

        offset += sprintf(buff+offset, ",algorithm=\"MD5\"\r\n");
	}
	else if (p_user->auth_mode == 0)	// basic auth
	{
		char auth[128] = {'\0'};
		char basic[256] = {'\0'};
		
		sprintf(auth, "%s:%s", p_user->user, p_user->pass);
		
		base64_encode((uint8 *)auth, (int)strlen(auth), basic, sizeof(basic));
		
		offset += sprintf(buff+offset, "Authorization: Basic %s\r\n", basic);
	}

	return offset;
}

BOOL http_cln_rx(HTTPREQ * p_user)
{
#ifdef HTTPS
	int pending;
#endif

	int rlen;
	HTTPMSG * rx_msg;

	if (p_user->p_rbuf == NULL) {
		p_user->p_rbuf = p_user->rcv_buf;
		p_user->mlen = sizeof(p_user->rcv_buf)-1;
		p_user->rcv_dlen = 0;
		p_user->ctt_len = 0;
		p_user->hdr_len = 0;
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
	if (rlen < 0) {
		log_print(H_LOG_INFO, "%s, recv return = %d, dlen[%d], mlen[%d]\r\n", __FUNCTION__, rlen, p_user->rcv_dlen, p_user->mlen);	

		closesocket(p_user->cfd);
		p_user->cfd = 0;
		return FALSE;
	}

	p_user->rcv_dlen += rlen;
	p_user->p_rbuf[p_user->rcv_dlen] = '\0';    

    if (0 == rlen) {
	    if (p_user->rcv_dlen < p_user->ctt_len + p_user->hdr_len && p_user->ctt_len == MAX_CTT_LEN) {
            // without Content-Length filed, when recv finish, fix the ctt length
            p_user->ctt_len = p_user->rcv_dlen - p_user->hdr_len;
        }
        else {
            log_print(H_LOG_INFO, "%s, recv return = %d, dlen[%d], mlen[%d]\r\n", __FUNCTION__, rlen, p_user->rcv_dlen, p_user->mlen);	

    		closesocket(p_user->cfd);
    		p_user->cfd = 0;
    		return FALSE;
        }
	}
	
// rx_analyse_point:
	if (p_user->rcv_dlen < 16) {
		return TRUE;
	}
	
	if (http_is_http_msg(p_user->p_rbuf) == FALSE) {
		return FALSE;
	}
	
	rx_msg = NULL;

	if (p_user->hdr_len == 0) {
		int parse_len;
		int http_pkt_len;

		http_pkt_len = http_pkt_find_end(p_user->p_rbuf);
		if (http_pkt_len == 0) {
			return TRUE;
		}
		p_user->hdr_len = http_pkt_len;

		rx_msg = http_get_msg_buf();
		if (rx_msg == NULL) {
			log_print(LOG_ERR, "%s, get_msg_buf ret null!!!\r\n", __FUNCTION__);
			return FALSE;
		}

		memcpy(rx_msg->msg_buf, p_user->p_rbuf, http_pkt_len);
		rx_msg->msg_buf[http_pkt_len] = '\0';
		
		log_print(LOG_DBG, "RX from %s << %s\r\n", p_user->host, rx_msg->msg_buf);
		
		parse_len = http_msg_parse_part1(rx_msg->msg_buf, http_pkt_len, rx_msg);
		if (parse_len != http_pkt_len) {
			log_print(LOG_ERR, "%s, http_msg_parse_part1=%d, http_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, http_pkt_len);

			http_free_msg(rx_msg);
			return FALSE;
		}
		
		p_user->ctt_len = rx_msg->ctt_len;
	}

    if (p_user->ctt_len == 0 && p_user->rcv_dlen > p_user->hdr_len) {
        // without Content-Length filed
        p_user->ctt_len = MAX_CTT_LEN;
    }
    
	if ((p_user->ctt_len + p_user->hdr_len) > p_user->mlen) {
		if (p_user->dyn_recv_buf) {
			free(p_user->dyn_recv_buf);
			p_user->dyn_recv_buf = NULL;
		}

		p_user->dyn_recv_buf = (char *)malloc(p_user->ctt_len + p_user->hdr_len + 1);
		if (NULL == p_user->dyn_recv_buf) {
		    if (rx_msg) {
		    	http_free_msg(rx_msg);
		    }
		    return FALSE;
		}
		
		memcpy(p_user->dyn_recv_buf, p_user->rcv_buf, p_user->rcv_dlen);
		p_user->p_rbuf = p_user->dyn_recv_buf;
		p_user->mlen   = p_user->ctt_len + p_user->hdr_len;

		if (rx_msg) {
			http_free_msg(rx_msg);
		}
		
#ifdef HTTPS 
		if (p_user->https) {
			pending = SSL_pending(p_user->ssl);
			if (pending > 0) {
        		goto READ;
			}
		}
#endif
		return TRUE;
	}

	if (p_user->rcv_dlen >= (p_user->ctt_len + p_user->hdr_len)) {
		if (rx_msg == NULL) {
			int nlen;
			int parse_len;

			nlen = p_user->ctt_len + p_user->hdr_len;
			if (nlen >= 2048) {
				rx_msg = http_get_msg_large_buf(nlen+1);
				if (rx_msg == NULL) {
				    log_print(LOG_ERR, "%s, http_get_msg_large_buf failed\r\n", __FUNCTION__);
					return FALSE;
				}	
			}
			else {
				rx_msg = http_get_msg_buf();
				if (rx_msg == NULL) {
				    log_print(LOG_ERR, "%s, http_get_msg_buf failed\r\n", __FUNCTION__);
					return FALSE;
				}	
			}

			memcpy(rx_msg->msg_buf, p_user->p_rbuf, p_user->hdr_len);
			rx_msg->msg_buf[p_user->hdr_len] = '\0';
			
			log_print(LOG_DBG, "RX from %s << %s\r\n", p_user->host, rx_msg->msg_buf);
			
			parse_len = http_msg_parse_part1(rx_msg->msg_buf, p_user->hdr_len, rx_msg);
			if (parse_len != p_user->hdr_len) {
				log_print(LOG_ERR, "%s, http_msg_parse_part1=%d, sip_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, p_user->hdr_len);

				http_free_msg(rx_msg);
				return FALSE;
			}				
		}

		if (p_user->ctt_len > 0) {
			int parse_len;
			
			memcpy(rx_msg->msg_buf+p_user->hdr_len, p_user->p_rbuf+p_user->hdr_len, p_user->ctt_len);
			rx_msg->msg_buf[p_user->hdr_len + p_user->ctt_len] = '\0';

			if (ctt_is_string(rx_msg->ctt_type)) {
				log_print(LOG_DBG, "%s\r\n\r\n", rx_msg->msg_buf+p_user->hdr_len);
			}
			
			parse_len = http_msg_parse_part2(rx_msg->msg_buf+p_user->hdr_len, p_user->ctt_len, rx_msg);
			if (parse_len != p_user->ctt_len) {
				log_print(LOG_ERR, "%s, http_msg_parse_part2=%d, sdp_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, p_user->ctt_len);

				http_free_msg(rx_msg);
				return FALSE;
			}
		}

		p_user->rx_msg = rx_msg;
	}

	if (p_user->rx_msg != rx_msg) {
        http_free_msg(rx_msg);
    }
	
	return TRUE;
}

BOOL http_cln_tx(HTTPREQ * p_req, const char * p_data, int len)
{
    int slen;
    
	if (p_req->cfd <= 0) {
		return FALSE;
    }
    
#ifdef HTTPS
	if (p_req->https)
		slen = SSL_write(p_req->ssl, p_data, len);
	else
		slen = send(p_req->cfd, p_data, len, 0);
#else	
	slen = send(p_req->cfd, p_data, len, 0);
#endif
	if (slen != len) {
	    log_print(LOG_ERR, "%s, slen = %d, len = %d\r\n", __FUNCTION__, slen, len);
		return FALSE;
    }
    
	return TRUE;
}

BOOL http_cln_rx_timeout(HTTPREQ * p_http, int timeout)
{
	BOOL ret = FALSE;
	int count = 0;
	int sret;
	fd_set fdr;
	struct timeval tv;
	
	while (1) {
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;
		
		FD_ZERO(&fdr);
		FD_SET(p_http->cfd, &fdr);

		sret = select((int)(p_http->cfd+1), &fdr, NULL, NULL, &tv);
		if (sret == 0) {
		    count++;
		    
		    if (count >= timeout / 100) {
		        log_print(H_LOG_WARN, "%s, timeout!!!\r\n", __FUNCTION__);
		        break;
		    }
		    
			continue;
		}
		else if (sret < 0) {
			log_print(LOG_ERR, "%s, select err[%s], sret[%d]!!!\r\n", __FUNCTION__, sys_os_get_socket_error(), sret);
			break;
		}
		
		if (FD_ISSET(p_http->cfd, &fdr)) {
			if (http_cln_rx(p_http) == FALSE) {
				break;
			}	
			else if (p_http->rx_msg != NULL) {
				ret = TRUE;
				break;
			}	
		}
	}

	return ret;
}

void http_cln_free_req(HTTPREQ * p_http)
{
	if (p_http->cfd > 0) {
		closesocket(p_http->cfd);
		p_http->cfd = 0;
	}

#ifdef HTTPS
	if (p_http->ssl) {
		SSL_free(p_http->ssl);
		p_http->ssl = NULL;
	}
#endif

	if (p_http->dyn_recv_buf) {
		free(p_http->dyn_recv_buf);
		p_http->dyn_recv_buf = NULL;
	}	
	
	if (p_http->rx_msg) {
		http_free_msg(p_http->rx_msg);
		p_http->rx_msg = NULL;
	}

	p_http->rcv_dlen  = 0;
	p_http->hdr_len   = 0;
	p_http->ctt_len   = 0;
	p_http->p_rbuf	  = NULL;
	p_http->mlen	  = 0;
	p_http->need_auth = FALSE;	
	p_http->auth_mode = 0;
	
	memset(p_http->rcv_buf, 0, sizeof(p_http->rcv_buf));
}
