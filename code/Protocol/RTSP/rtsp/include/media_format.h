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

#ifndef _MEDIA_FORMAT_H
#define _MEDIA_FORMAT_H

// video pixel format
#define VIDEO_FMT_NONE          0
#define VIDEO_FMT_YUYV422       1
#define VIDEO_FMT_YUV420P       2
#define VIDEO_FMT_YVYU422       3
#define VIDEO_FMT_UYVY422       4
#define VIDEO_FMT_NV12          5
#define VIDEO_FMT_NV21          6
#define VIDEO_FMT_RGB24         7
#define VIDEO_FMT_RGB32         8
#define VIDEO_FMT_ARGB          9
#define VIDEO_FMT_BGRA          10

// video codec
#define VIDEO_CODEC_NONE        0
#define VIDEO_CODEC_H264        1
#define VIDEO_CODEC_MP4         2
#define VIDEO_CODEC_JPEG        3
#define VIDEO_CODEC_H265        4

// audio codec
#define AUDIO_CODEC_NONE        0
#define AUDIO_CODEC_G711A       1
#define AUDIO_CODEC_G711U       2
#define AUDIO_CODEC_G726        3
#define AUDIO_CODEC_AAC         4
#define AUDIO_CODEC_G722        5
#define AUDIO_CODEC_OPUS        6


#define DATA_TYPE_AUDIO    0
#define DATA_TYPE_VIDEO    1
#define DATA_TYPE_METADATA 2
#define DATA_TYPE_IMAGE    3

#endif // _MEDIA_FORMAT_H


