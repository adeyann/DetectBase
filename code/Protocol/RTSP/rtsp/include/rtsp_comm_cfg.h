
#ifndef COMM_CFG_H
#define COMM_CFG_H

typedef struct
{
    int     codec;                  // video codec, refer media_format.h
    int     width;                  // video width
    int     height;                 // video height
    int     framerate;              // frame rate
    int     bitrate;                // bitrate
} RTSP_V_INFO;

typedef struct
{
    int     codec;                  // audio codec, refer media_format.h
    int     samplerate;             // sample rate
    int     channels;               // channels
    int     bitrate;                // bitrate
} RTSP_A_INFO;

typedef struct _RTSP_OUTPUT
{
    struct _RTSP_OUTPUT * next;

    char            url[256];       // the match url path
    RTSP_V_INFO     v_info;         // video output information
	RTSP_A_INFO     a_info;         // audio output information

} RTSP_OUTPUT;

#endif


