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
#include "rfc_md5.h"
#include "rtsp_auth.h"


/* calculate H(A1) as per spec */
void DigestCalcHA1(
	IN const char * pszAlg,
	IN const char * pszUserName,
	IN const char * pszRealm,
	IN const char * pszPassword,
	IN const char * pszNonce,
	IN const char * pszCNonce,
	OUT HASHHEX SessionKey
)
{
	MD5_CTX Md5Ctx;
	HASH HA1;

	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)pszUserName, strlen(pszUserName));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)pszRealm, strlen(pszRealm));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)pszPassword, strlen(pszPassword));
	MD5Final(HA1, &Md5Ctx);

	if (strcmp(pszAlg, "md5-sess") == 0) 
	{
		MD5Init(&Md5Ctx);
		MD5Update(&Md5Ctx, HA1, HASHLEN);
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)pszNonce, strlen(pszNonce));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)pszCNonce, strlen(pszCNonce));
		MD5Final(HA1, &Md5Ctx);
	};

	BinToHexStr(HA1, SessionKey);
};

/* calculate request-digest/response-digest as per HTTP Digest spec */
void DigestCalcResponseHash(
	IN HASHHEX HA1, /* H(A1) */
	IN const char * pszNonce, /* nonce from server */
	IN const char * pszNonceCount, /* 8 hex digits */
	IN const char * pszCNonce, /* client nonce */
	IN const char * pszQop, /* qop-value: "", "auth", "auth-int" */
	IN const char * pszMethod, /* method from the request */
	IN const char * pszDigestUri, /* requested URL */
	IN HASHHEX HEntity, /* H(entity body) if qop="auth-int" */
	OUT HASH  RespHash /* request-digest or response-digest */
)
{
	MD5_CTX Md5Ctx;
	HASH HA2;
	HASHHEX HA2Hex;

	// calculate H(A2)
	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)pszMethod, strlen(pszMethod));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)pszDigestUri, strlen(pszDigestUri));

	if (strcmp(pszQop, "auth-int") == 0) 
	{
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)(&HEntity[0]), HASHHEXLEN);
	};
	
	MD5Final(HA2, &Md5Ctx);
	BinToHexStr(HA2, HA2Hex);

	// calculate response
	MD5Init(&Md5Ctx);
	MD5Update(&Md5Ctx, (uint8 *)(&HA1[0]), HASHHEXLEN);
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	MD5Update(&Md5Ctx, (uint8 *)pszNonce, strlen(pszNonce));
	MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);

	if (*pszQop) 
	{
		MD5Update(&Md5Ctx, (uint8 *)pszNonceCount, strlen(pszNonceCount));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)pszCNonce, strlen(pszCNonce));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
		MD5Update(&Md5Ctx, (uint8 *)pszQop, strlen(pszQop));
		MD5Update(&Md5Ctx, (uint8 *)&(":"), 1);
	};
	
	MD5Update(&Md5Ctx, (uint8 *)(&HA2Hex[0]), HASHHEXLEN);
	MD5Final(RespHash, &Md5Ctx);
};

/* calculate request-digest/response-digest as per HTTP Digest spec */
void DigestCalcResponse(
	IN HASHHEX HA1, /* H(A1) */
	IN const char * pszNonce, /* nonce from server */
	IN const char * pszNonceCount, /* 8 hex digits */
	IN const char * pszCNonce, /* client nonce */
	IN const char * pszQop, /* qop-value: "", "auth", "auth-int" */
	IN const char * pszMethod, /* method from the request */
	IN const char * pszDigestUri, /* requested URL */
	IN HASHHEX HEntity, /* H(entity body) if qop="auth-int" */
	OUT HASHHEX Response /* request-digest or response-digest */
)
{
	HASH  RespHash;

    DigestCalcResponseHash(HA1, pszNonce, pszNonceCount, pszCNonce, pszQop, pszMethod, pszDigestUri, HEntity, RespHash);
	BinToHexStr(RespHash, Response);
};






