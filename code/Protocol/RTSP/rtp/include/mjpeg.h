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

#ifndef MJPEG_H
#define MJPEG_H

/* JPEG marker codes */
enum JpegMarker 
{
    /* start of frame */
	MARKER_SOF0  = 0xc0,	   /* start-of-frame, baseline scan */
    MARKER_SOF1  = 0xc1,       /* extended sequential, huffman */
    MARKER_SOF2  = 0xc2,       /* progressive, huffman */
    MARKER_SOF3  = 0xc3,       /* lossless, huffman */

    MARKER_SOF5  = 0xc5,       /* differential sequential, huffman */
    MARKER_SOF6  = 0xc6,       /* differential progressive, huffman */
    MARKER_SOF7  = 0xc7,       /* differential lossless, huffman */
    MARKER_JPG   = 0xc8,       /* reserved for JPEG extension */
    MARKER_SOF9  = 0xc9,       /* extended sequential, arithmetic */
    MARKER_SOF10 = 0xca,       /* progressive, arithmetic */
    MARKER_SOF11 = 0xcb,       /* lossless, arithmetic */

    MARKER_SOF13 = 0xcd,       /* differential sequential, arithmetic */
    MARKER_SOF14 = 0xce,       /* differential progressive, arithmetic */
    MARKER_SOF15 = 0xcf,       /* differential lossless, arithmetic */

    MARKER_DHT   = 0xc4,       /* define huffman tables */

    MARKER_DAC   = 0xcc,       /* define arithmetic-coding conditioning */

    /* restart with modulo 8 count "m" */
    MARKER_RST0  = 0xd0,
    MARKER_RST1  = 0xd1,
    MARKER_RST2  = 0xd2,
    MARKER_RST3  = 0xd3,
    MARKER_RST4  = 0xd4,
    MARKER_RST5  = 0xd5,
    MARKER_RST6  = 0xd6,
    MARKER_RST7  = 0xd7,

	MARKER_SOI	 = 0xd8,	   /* start of image */
	MARKER_EOI	 = 0xd9,	   /* end of image */
	MARKER_SOS	 = 0xda,	   /* start of scan */
    MARKER_DQT   = 0xdb,       /* define quantization tables */
    MARKER_DNL   = 0xdc,       /* define number of lines */
	MARKER_DRI	 = 0xdd,	   /* restart interval */
    MARKER_DHP   = 0xde,       /* define hierarchical progression */
    MARKER_EXP   = 0xdf,       /* expand reference components */

	MARKER_APP_FIRST= 0xe0,
    MARKER_APP1  = 0xe1,
    MARKER_APP2  = 0xe2,
    MARKER_APP3  = 0xe3,
    MARKER_APP4  = 0xe4,
    MARKER_APP5  = 0xe5,
    MARKER_APP6  = 0xe6,
    MARKER_APP7  = 0xe7,
    MARKER_APP8  = 0xe8,
    MARKER_APP9  = 0xe9,
    MARKER_APP10 = 0xea,
    MARKER_APP11 = 0xeb,
    MARKER_APP12 = 0xec,
    MARKER_APP13 = 0xed,
    MARKER_APP14 = 0xee,
	MARKER_APP_LAST	= 0xef,

    MARKER_JPG0  = 0xf0,
    MARKER_JPG1  = 0xf1,
    MARKER_JPG2  = 0xf2,
    MARKER_JPG3  = 0xf3,
    MARKER_JPG4  = 0xf4,
    MARKER_JPG5  = 0xf5,
    MARKER_JPG6  = 0xf6,
    MARKER_SOF48 = 0xf7,
    MARKER_LSE   = 0xf8, 
    MARKER_JPG9  = 0xf9,
    MARKER_JPG10 = 0xfa,
    MARKER_JPG11 = 0xfb,
    MARKER_JPG12 = 0xfc,
    MARKER_JPG13 = 0xfd,

	MARKER_COMMENT	= 0xfe,

    MARKER_TEM   = 0x01,       /* temporary private use for arithmetic coding */

    /* 0x02 -> 0xbf reserved */
};

#endif /* MJPEG_H */


