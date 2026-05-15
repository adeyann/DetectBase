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

#ifndef HTTP_CLN_H
#define HTTP_CLN_H


#ifdef __cplusplus
extern "C" {
#endif

BOOL http_get_digest_info(HTTPMSG * rx_msg, HD_AUTH_INFO * auth_info);
BOOL http_calc_auth_digest(HD_AUTH_INFO * auth_info, const char * method);
int  http_build_auth_msg(HTTPREQ * p_user, const char * method, char * buff);

BOOL http_cln_rx(HTTPREQ * p_user);
BOOL http_cln_tx(HTTPREQ * p_user, const char * p_data, int len);
BOOL http_cln_rx_timeout(HTTPREQ * p_http, int timeout);
void http_cln_free_req(HTTPREQ * p_http);

#ifdef __cplusplus
}
#endif

#endif


