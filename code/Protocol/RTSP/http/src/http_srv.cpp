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
#include "http_srv.h"
#include "rtsp_http.h"
#include "rtsp_rsua.h"
#include "rtsp_srv.h"
#include <memory>

#ifdef RTSP_OVER_HTTP

/***************************************************************************************/
extern RTSP_CLASS	hrtsp;

/***************************************************************************************/
void http_commit_rx_msg(HTTPCLN * p_user, HTTPMSG * rx_msg)
{
	if (!rtsp_http_process(p_user, rx_msg)) {
	    closesocket(p_user->cfd);
	    p_user->cfd = 0;
	}
}

void http_commit_rtsp_msg(HTTPCLN * p_user, char * p_buff, int len)
{
    if (!rtsp_http_msg_process(p_user, p_buff, len)) {
        closesocket(p_user->cfd);
	    p_user->cfd = 0;
    }
}

BOOL http_srv_rx(HTTPCLN * p_user)
{
	int rlen;
    HTTPMSG * rx_msg = NULL;

	if (p_user->p_rbuf == NULL) {
		p_user->p_rbuf   = p_user->rcv_buf;
		p_user->mlen     = sizeof(p_user->rcv_buf)-1;
		p_user->rcv_dlen = 0;
		p_user->ctt_len  = 0;
		p_user->hdr_len  = 0;
	}

	rlen = recv(p_user->cfd, p_user->p_rbuf+p_user->rcv_dlen, p_user->mlen-p_user->rcv_dlen, 0);
	if (rlen <= 0) {
		log_print(H_LOG_WARN, "%s, recv return = %d, dlen[%d], mlen[%d]\r\n", __FUNCTION__, rlen, p_user->rcv_dlen, p_user->mlen);
		return FALSE;
	}

	p_user->rcv_dlen += rlen;
	p_user->p_rbuf[p_user->rcv_dlen] = '\0';

    if (p_user->p_rua) {
        RSUA * p_rua = (RSUA *)p_user->p_rua;

        if (p_user == p_rua->rtsp_cmd) {
            http_commit_rtsp_msg(p_user, p_user->p_rbuf, p_user->rcv_dlen);
        }
        else if (p_user == p_rua->rtsp_data) {
            // rtcp message, skip
        }

        p_user->hdr_len  = 0;
		p_user->ctt_len  = 0;
		p_user->p_rbuf   = 0;
		p_user->rcv_dlen = 0;

        return TRUE;
    }

rx_analyse_point:

    if (rx_msg) {
		http_free_msg(rx_msg);
		rx_msg = NULL;
	}

	if (p_user->rcv_dlen < 16)
		return TRUE;

	if (http_is_http_msg(p_user->p_rbuf) == FALSE)
		return FALSE;

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

		log_print(LOG_DBG, "RX << %s\r\n", rx_msg->msg_buf);

		parse_len = http_msg_parse_part1(rx_msg->msg_buf, http_pkt_len, rx_msg);
		if (parse_len != http_pkt_len) {
			log_print(LOG_ERR, "%s, http_msg_parse_part1=%d, http_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, http_pkt_len);
			http_free_msg(rx_msg);
			return FALSE;
		}

		p_user->ctt_len  = rx_msg->ctt_len;
		p_user->ctt_type = rx_msg->ctt_type;

		if (CTT_RTSP_TUNNELLED == p_user->ctt_type) {
            http_commit_rx_msg(p_user, rx_msg);

            if (p_user->rcv_dlen - http_pkt_len > 0)
                http_commit_rtsp_msg(p_user, p_user->p_rbuf+p_user->hdr_len, p_user->rcv_dlen-http_pkt_len);

            p_user->hdr_len  = 0;
			p_user->ctt_len  = 0;
			p_user->p_rbuf   = 0;
			p_user->rcv_dlen = 0;

			if (rx_msg)
                http_free_msg(rx_msg);

            return TRUE;
        }
	}

	if ((p_user->ctt_len + p_user->hdr_len) > p_user->mlen) {
		if (p_user->dyn_recv_buf) {
			log_print(H_LOG_INFO, "%s, dyn_recv_buf=%p, mlen=%d!!!\r\n", __FUNCTION__, p_user->dyn_recv_buf, p_user->mlen);
			free(p_user->dyn_recv_buf);
		}

		p_user->dyn_recv_buf = (char *)malloc(p_user->ctt_len + p_user->hdr_len + 1);
		if (NULL == p_user->dyn_recv_buf) {
		    if (rx_msg)
    		    http_free_msg(rx_msg);

		    log_print(H_LOG_INFO, "%s, malloc failed\r\n", __FUNCTION__);
		    return FALSE;
		}

		memcpy(p_user->dyn_recv_buf, p_user->rcv_buf, p_user->rcv_dlen);

		p_user->p_rbuf = p_user->dyn_recv_buf;
		p_user->mlen   = p_user->ctt_len + p_user->hdr_len;

		if (rx_msg)
			http_free_msg(rx_msg);

		return TRUE;
	}

	if (p_user->rcv_dlen >= (p_user->ctt_len + p_user->hdr_len)) {
		if (rx_msg == NULL) {
		    int parse_len;
			int nlen;

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

			log_print(LOG_DBG, "RX << %s\r\n\r\n", rx_msg->msg_buf);

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

			if (ctt_is_string(rx_msg->ctt_type))
				log_print(LOG_DBG, "%s\r\n\r\n", rx_msg->msg_buf+p_user->hdr_len);

			parse_len = http_msg_parse_part2(rx_msg->msg_buf+p_user->hdr_len, p_user->ctt_len, rx_msg);
			if (parse_len != p_user->ctt_len)
				log_print(H_LOG_WARN, "%s, http_msg_parse_part2=%d, sdp_pkt_len=%d!!!\r\n", __FUNCTION__, parse_len, p_user->ctt_len);
		}

		http_commit_rx_msg(p_user, rx_msg);

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
			p_user->hdr_len      = 0;
			p_user->ctt_len      = 0;
			p_user->p_rbuf       = 0;
			p_user->rcv_dlen     = 0;
		}
	}

    if (rx_msg)
        http_free_msg(rx_msg);

    return TRUE;
}

int http_listen_rx(HTTPSRV * p_srv)
{
    SOCKET cfd;
	struct sockaddr_in caddr;
	socklen_t size;
	HTTPCLN * p_cln;

    size = sizeof(struct sockaddr_in);
	cfd  = accept(p_srv->sfd, (struct sockaddr *)&caddr, &size);
	if (cfd <= 0) {
		log_print(LOG_ERR, "%s, accept, cfd(%d), %s\r\n", __FUNCTION__, cfd, sys_os_get_socket_error());
		return -1;
	}

	p_cln = http_get_idle_cln(p_srv);
	if (p_cln == NULL) {
		log_print(LOG_ERR, "%s, http_get_idle_cln::ret null!!!\r\n", __FUNCTION__);
		closesocket(cfd);
		return -1;
	}

	int len = 1024 * 1024;
	if (setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, (char*)&len, sizeof(int))) {
		log_print(H_LOG_WARN, "%s, setsockopt SO_SNDBUF error!!!\r\n", __FUNCTION__);
	}
	if (setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, (char*)&len, sizeof(int))) {
		log_print(H_LOG_WARN, "%s, setsockopt SO_SNDBUF error!!!\r\n", __FUNCTION__);
	}

	p_cln->cfd = cfd;
	p_cln->rip = caddr.sin_addr.s_addr;
	p_cln->rport = ntohs(caddr.sin_port);

	pps_ctx_ul_add(p_srv->cln_ul, p_cln);

	log_print(H_LOG_INFO, "http user over tcp from[0x%08x,%u]\r\n", p_cln->rip, p_cln->rport);

	return 0;
}

void * http_rx_thread(void * argv)
{
	fd_set fdr;
	HTTPSRV * p_srv = (HTTPSRV *)argv;
	if (p_srv == NULL)
	{
		return NULL;
	}

	log_print(LOG_DBG, "%s, start\r\n", __FUNCTION__);

	auto&  running = p_srv->rxThread.GetRunningFlag();
	while( running.load() == true )
	{
	    int sret;
	    int max_fd;
	    HTTPCLN * p_cln;
	    struct timeval tv;

		FD_ZERO(&fdr);
		FD_SET(p_srv->sfd, &fdr);
		max_fd = (int)p_srv->sfd;

		p_cln = (HTTPCLN *)pps_lookup_start(p_srv->cln_ul);
		while (p_cln) {
			if (p_cln->cfd > 0) {
				FD_SET(p_cln->cfd, &fdr);
				max_fd = (int)(((int)p_cln->cfd > max_fd) ? p_cln->cfd : max_fd);
			}
			else {
				HTTPCLN * p_next_cln = (HTTPCLN *)pps_ctx_ul_del_unlock(p_srv->cln_ul, p_cln);

                http_free_used_cln(p_srv, p_cln);
                p_cln = p_next_cln;
                continue;
			}

			p_cln = (HTTPCLN *)pps_lookup_next(p_srv->cln_ul, p_cln);
		}
		pps_lookup_end(p_srv->cln_ul);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		sret = select(max_fd+1, &fdr,NULL,NULL,&tv);
		if (sret == 0) {
			continue;
		}
		else if (sret < 0) {
			log_print(LOG_ERR, "%s, select err[%s], max fd[%d], sret[%d]!!!\r\n", __FUNCTION__, sys_os_get_socket_error(), max_fd, sret);
			continue;
		}

		if (FD_ISSET(p_srv->sfd, &fdr))
			http_listen_rx(p_srv);

		p_cln = (HTTPCLN *)pps_lookup_start(p_srv->cln_ul);
		while (p_cln) {
			if (p_cln->cfd > 0 && FD_ISSET(p_cln->cfd, &fdr)) {
				if (http_srv_rx(p_cln) == FALSE) {
					HTTPCLN * p_next_cln = (HTTPCLN *)pps_ctx_ul_del_unlock(p_srv->cln_ul, p_cln);

                    http_free_used_cln(p_srv, p_cln);

					p_cln = p_next_cln;
					continue;
				}
			}
			else if (p_cln->cfd == 0) {
			    HTTPCLN * p_next_cln = (HTTPCLN *)pps_ctx_ul_del_unlock(p_srv->cln_ul, p_cln);

                http_free_used_cln(p_srv, p_cln);

			    p_cln = p_next_cln;
			    continue;
			}

			p_cln = (HTTPCLN *)pps_lookup_next(p_srv->cln_ul, p_cln);
		}
		pps_lookup_end(p_srv->cln_ul);
	}

	log_print(LOG_DBG, "%s, exit\r\n", __FUNCTION__);

	return NULL;
}

int http_srv_net_init(HTTPSRV * p_srv)
{
    int val = 1;
    int reuse_ret;
    struct sockaddr_in addr;

	p_srv->sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (p_srv->sfd <= 0) {
		log_print(LOG_ERR, "%s, socket err[%s]!!!\r\n", __FUNCTION__, sys_os_get_socket_error());
		return -1;
	}

	reuse_ret = setsockopt(p_srv->sfd, SOL_SOCKET, SO_REUSEADDR, (char *)&val, 4);
	UNUSED( reuse_ret );

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = p_srv->saddr;
	addr.sin_port = htons(p_srv->sport);

	if (bind(p_srv->sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		log_print(LOG_ERR, "%s, bind tcp socket fail,err[%s]!!!\n", __FUNCTION__, sys_os_get_socket_error());
		closesocket(p_srv->sfd);
		p_srv->sfd = 0;
		return -1;
	}

	if (listen(p_srv->sfd, 10) < 0) {
		log_print(LOG_ERR, "%s, listen tcp socket fail,err[%s]!!!\r\n", __FUNCTION__, sys_os_get_socket_error());
		closesocket(p_srv->sfd);
		return -1;
	}

	return 0;
}

int http_srv_init(HTTPSRV * p_srv, uint32 saddr, uint16 sport, int cln_num)
{
	memset(p_srv, 0, sizeof(HTTPSRV));

	p_srv->saddr = saddr;
	p_srv->sport = sport;

	p_srv->cln_fl = pps_ctx_fl_init(cln_num, sizeof(HTTPCLN), TRUE);
	if (p_srv->cln_fl == NULL)
		return -1;

	p_srv->cln_ul = pps_ctx_ul_init(p_srv->cln_fl, TRUE);
	if (p_srv->cln_ul == NULL)
		return -1;

	if (http_srv_net_init(p_srv) != 0)
		return -1;

	p_srv->r_flag   = 1;
	p_srv->rxThread.SetThreadFunctions(
		[p_srv]() {
			void* ret = http_rx_thread( p_srv );
		}
	);
	p_srv->rxThread.Start();

	return 0;
}

uint32 http_cln_index(HTTPSRV * p_srv, HTTPCLN * p_cln)
{
	return pps_get_index(p_srv->cln_fl, p_cln);
}

HTTPCLN * http_get_cln_by_index(HTTPSRV * p_srv, unsigned long index)
{
	return (HTTPCLN *)pps_get_node_by_index(p_srv->cln_fl, index);
}

HTTPCLN * http_get_idle_cln(HTTPSRV * p_srv)
{
	HTTPCLN * p_cln = (HTTPCLN *)pps_fl_pop(p_srv->cln_fl);
	if (p_cln) {
		memset(p_cln, 0, sizeof(HTTPCLN));
		p_cln->guid = p_srv->guid++;
	}

	return p_cln;
}

void http_free_used_cln(HTTPSRV * p_srv, HTTPCLN * p_cln)
{
	if (p_cln->dyn_recv_buf) {
		free(p_cln->dyn_recv_buf);
		p_cln->dyn_recv_buf = NULL;
	}

	if (p_cln->cfd > 0) {
		closesocket(p_cln->cfd);
		p_cln->cfd = 0;
	}

	if (p_cln->p_rua) {
	    RSUA * p_rua = (RSUA *)p_cln->p_rua;

	    if (p_rua->rtsp_cmd)
	        ((HTTPCLN *)p_rua->rtsp_cmd)->p_rua = NULL;

	    if (p_rua->rtsp_data)
	        ((HTTPCLN *)p_rua->rtsp_data)->p_rua = NULL;

	    RIMSG msg;
    	memset(&msg,0,sizeof(RIMSG));
    	msg.msg_src = RTSP_DEL_UA_SRC;
    	msg.msg_dua = rua_get_index(p_rua);
    	msg.msg_buf = NULL;

    	if (hqBufPut(hrtsp.msg_queue, (char *)&msg) == FALSE)
    		log_print(LOG_ERR, "rtsp_stop_rua::send msg[NULL] to main task failed!!!\r\n");
	}

	pps_fl_push_tail(p_srv->cln_fl, p_cln);
}

void http_srv_deinit(HTTPSRV * p_srv)
{
    p_srv->r_flag = 0;
	p_srv->rxThread.Stop();

	if (p_srv->cln_ul) {
		pps_ul_free(p_srv->cln_ul);
		p_srv->cln_ul = NULL;
	}

	if (p_srv->cln_fl) {
		pps_fl_free(p_srv->cln_fl);
		p_srv->cln_fl = NULL;
	}

    if (p_srv->sfd > 0) {
    	closesocket(p_srv->sfd);
    	p_srv->sfd = 0;
	}
}
#endif // end of RTSP_OVER_HTTP



