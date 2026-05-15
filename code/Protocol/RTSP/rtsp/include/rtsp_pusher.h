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

#ifndef RTSP_PUSHER_H
#define RTSP_PUSHER_H


#include "linked_list.h"
#include "hqueue.h"
#include "rtsp_comm_cfg.h"
#include "h264_rtp_rx.h"
#include "h265_rtp_rx.h"
#include "mjpeg_rtp_rx.h"
#include "mpeg4_rtp_rx.h"
#include "pcm_rtp_rx.h"
#include "aac_rtp_rx.h"
#include "rtp.h"

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
#include "video_decoder.h"
#include "audio_decoder.h"
#include "video_encoder.h"
#include "audio_encoder.h"
#endif


#define MAX_RTP_LEN         (64*1024)

#define TRANSFER_MODE_TCP   0
#define TRANSFER_MODE_UDP   1
#define TRANSFER_MODE_RTSP  2

#define MEDIA_TYPE_VIDEO    0
#define MEDIA_TYPE_AUDIO    1

class CRtspPusher;

typedef struct
{
    int             mode;
    uint32          ip;
    int             vport;
    int             aport;
} RTSP_TRANSFER;

typedef struct
{
    uint32          has_video : 1;
    uint32          has_audio : 1;
    uint32          has_output: 1;
    uint32          reserved  : 29;
    
    char            suffix[100];
    
    RTSP_V_INFO     v_info;         // video input information
    RTSP_A_INFO     a_info;         // audio input information

    RTSP_TRANSFER   transfer;       // transfer information
    
    RTSP_OUTPUT     output;         // output information
} PUSHER_CFG;

typedef struct _RTSP_PUSHER
{
    struct _RTSP_PUSHER * next;
    
    PUSHER_CFG      cfg;
    
    CRtspPusher *   pusher;
} RTSP_PUSHER;

typedef void (*PusherCallback)(uint8 * data, int size, int type, void * pUserdata);

typedef struct
{
	PusherCallback  pCallback;
	void *          pUserdata;
	BOOL            bFirst;
} PusherCB;

typedef struct
{
	int	            sfd;					// listen socket 
	int		        mfd;					// data send and receive socket, only for TCP mode
    int             rtp_pt;                 // rtp payload type
    
	char	        recv_buf[16];			// receive buffer 
	char *			dyn_recv_buf;           // dynamic receive buffer
	uint32	        recv_data_offset;		// receive buffer offset 

	union {
	    H264RXI     h264rxi;
	    H265RXI     h265rxi;
	    MJPEGRXI    mjpegrxi;
	    MPEG4RXI    mpeg4rxi;

        AACRXI      aacrxi;
	    PCMRXI      pcmrxi;	    
	};
} PUA;


#ifdef __cplusplus
extern "C" {
#endif

RTSP_PUSHER* rtsp_add_pusher(RTSP_PUSHER ** p_pusher);
void         rtsp_free_pushers(RTSP_PUSHER ** p_pusher);
BOOL         rtsp_init_pusher(RTSP_PUSHER * p_pusher);
void         rtsp_init_pushers();
int          rtsp_get_pusher_nums();
RTSP_PUSHER* rtsp_pusher_match(const char * suffix);

#ifdef __cplusplus
}
#endif

class CRtspPusher
{
public:
    CRtspPusher(PUSHER_CFG * pConfig);
    ~CRtspPusher(void);

public:
        
    char *  getVideoAuxSDPLine(int rtp_pt);
    char *  getAudioAuxSDPLine(int rtp_pt);

    void    addCallback(PusherCallback pCallback, void * pUserdata);
    void    delCallback(PusherCallback pCallback, void * pUserdata);

    BOOL    isInited() {return m_bInited;}   
    PUSHER_CFG * getConfig() {return m_pConfig;}
    
    void    tcpRecvThread();
    void    udpRecvThread();
    
    void    videoDataRx(uint8 * p_data, int len);
    void    audioDataRx(uint8 * p_data, int len);

    /************************************************************/
    BOOL    startUdpRx(int vfd, int afd);
    void    stopUdpRx();
    void    setRua(void * p_rua);
    void  * getRua() {return m_rua;}
    void    setAACConfig(int size_length, int index_length, int index_delta_length);
    void    setMpeg4Config(uint8 * data, int len);
    void    setH264Params(char * p_sdp);
    void    setH265Params(char * p_sdp);
    BOOL    videoRtpRx(uint8 * p_rtp, int rlen);
    BOOL    audioRtpRx(uint8 * p_rtp, int rlen);
    void    setVideoInfo(int has_video, int codec);
    void    setAudioInfo(int has_audio, int codec, int samplerate, int channels);
    int     parseVideoSize(uint8 * p_data, int len); 
    
#if defined(RTSP_FILE) || defined(RTSP_DEVICE)    
    int     getVideoRecodec() {return m_nVideoRecodec;}
    int     getAudioRecodec() {return m_nAudioRecodec;}
    int     needVideoRecodec(uint8 * p_data, int len);
    int     needAudioRecodec();
    void    videoDataDecode(uint8 * p_data, int len);
    void    videoDataEncode(AVFrame * frame);
    void    audioDataDecode(uint8 * p_data, int len);
    void    audioDataEncode(AVFrame * frame);
#endif

private:
    void    initPusher();
    int     initSocket(uint32 ip, int port, int mode);
    void    reinitMedia();
    BOOL    startPusher();

    void    tcpListenRx(PUA * p_ua);
    BOOL    tcpDataRx(PUA * p_ua, int type);
    int     tcpPacketRx(PUA * p_ua, RILF ** ppRtPkt);
    BOOL    videoRtpRx(PUA * p_ua, uint8 * p_rtp, int rlen);
    BOOL    audioRtpRx(PUA * p_ua, uint8 * p_rtp, int rlen);
    BOOL    udpDataRx(PUA * p_ua, int type);
    void    closeMedia(PUA * p_ua);
    
    void    dataCallback(uint8 * data, int size, int type);
    BOOL    isCallbackExist(PusherCallback pCallback, void *pUserdata);
    char *  getH264AuxSDPLine(int rtp_pt);
    char *  getH265AuxSDPLine(int rtp_pt);
    char *  getMP4AuxSDPLine(int rtp_pt);
    char *  getAACAuxSDPLine(int rtp_pt);

private:
    PUSHER_CFG    * m_pConfig;
    BOOL            m_bInited;

    PUA             m_vua;
    PUA             m_aua;
	
    void          * m_pRuaMutex;
    void          * m_rua;

    BOOL            m_bRecving;
    pthread_t       m_tidRecv;
    
	void          * m_pCallbackMutex;
	LINKED_LIST   * m_pCallbackList;

#if defined(RTSP_FILE) || defined(RTSP_DEVICE)
    int             m_nVideoRecodec;    // 1  - need; 0  - Still not sure; -1 - not need
    int             m_nAudioRecodec;    // 1  - need; 0  - Still not sure; -1 - not need
    
    CVideoDecoder * m_pVideoDecoder;    // video decoder
    CAudioDecoder * m_pAudioDecoder;    // audio decoder
	CVideoEncoder * m_pVideoEncoder;    // video encoder  
	CAudioEncoder * m_pAudioEncoder;    // audio encoder
#endif	
};

#endif



