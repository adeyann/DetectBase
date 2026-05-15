
#ifndef RTSP_CRYPT_H
#define RTSP_CRYPT_H


#ifdef __cplusplus
extern "C" {
#endif

void rtsp_crypt_init();
void rtsp_crypt_uninit();
BOOL rtsp_crypt_data_decrypt(uint8 * p_data, int len);

#ifdef __cplusplus
}
#endif

#endif


