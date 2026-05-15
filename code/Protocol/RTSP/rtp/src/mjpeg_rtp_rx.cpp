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
#include "mjpeg.h"
#include "mjpeg_rtp_rx.h"
#include "mjpeg_tables.h"


static void mjpeg_create_huffman_header
(
uint8*& p,
uint8 const* codelens,
int ncodes,
uint8 const* symbols,
int nsymbols,
int tableNo, int tableClass
) 
{
	*p++ = 0xff; *p++ = MARKER_DHT;
	*p++ = 0;               		/* length msb */
	*p++ = 3 + ncodes + nsymbols; 	/* length lsb */
	*p++ = (tableClass << 4) | tableNo;
	memcpy(p, codelens, ncodes);
	p += ncodes;
	memcpy(p, symbols, nsymbols);
	p += nsymbols;
}

uint32 mjpeg_compute_header_size(uint32 qtlen, uint32 dri) 
{
	uint32 qtlen_half = qtlen/2; // in case qtlen is odd; shouldn't happen
	qtlen = qtlen_half*2;

	uint32 numQtables = qtlen > 64 ? 2 : 1;
	return 485 + numQtables*5 + qtlen + (dri > 0 ? 6 : 0);
}

int mjpeg_create_header
(
uint8* buf, uint32 type,
uint32 w, uint32 h,
uint8 const* qtables, uint32 qtlen,
uint32 dri
) 
{
	uint8 *ptr = buf;
	uint32 numQtables = qtlen > 64 ? 2 : 1;

	// MARKER_SOI:
	*ptr++ = 0xFF; *ptr++ = MARKER_SOI;

	// MARKER_APP_FIRST:
	*ptr++ = 0xFF; *ptr++ = MARKER_APP_FIRST;
	*ptr++ = 0x00; *ptr++ = 0x10; // size of chunk
	*ptr++ = 'J'; *ptr++ = 'F'; *ptr++ = 'I'; *ptr++ = 'F'; *ptr++ = 0x00;
	*ptr++ = 0x01; *ptr++ = 0x01; // JFIF format version (1.1)
	*ptr++ = 0x00; // no units
	*ptr++ = 0x00; *ptr++ = 0x01; // Horizontal pixel aspect ratio
	*ptr++ = 0x00; *ptr++ = 0x01; // Vertical pixel aspect ratio
	*ptr++ = 0x00; *ptr++ = 0x00; // no thumbnail

	// MARKER_DRI:
	if (dri > 0) 
	{
		*ptr++ = 0xFF; *ptr++ = MARKER_DRI;
		*ptr++ = 0x00; *ptr++ = 0x04; // size of chunk
		*ptr++ = (uint8)(dri >> 8); *ptr++ = (uint8)(dri); // restart interval
	}

	// MARKER_DQT (luma):
	uint32 tableSize = numQtables == 1 ? qtlen : qtlen/2;
	*ptr++ = 0xFF; *ptr++ = MARKER_DQT;
	*ptr++ = 0x00; *ptr++ = tableSize + 3; // size of chunk
	*ptr++ = 0x00; // precision(0), table id(0)
	memcpy(ptr, qtables, tableSize);
	qtables += tableSize;
	ptr += tableSize;

	if (numQtables > 1) 
	{
		tableSize = qtlen - qtlen/2;
		// MARKER_DQT (chroma):
		*ptr++ = 0xFF; *ptr++ = MARKER_DQT;
		*ptr++ = 0x00; *ptr++ = tableSize + 3; // size of chunk
		*ptr++ = 0x01; // precision(0), table id(1)
		memcpy(ptr, qtables, tableSize);
		qtables += tableSize;
		ptr += tableSize;
	}

	// MARKER_SOF0:
	*ptr++ = 0xFF; *ptr++ = MARKER_SOF0;
	*ptr++ = 0x00; *ptr++ = 0x11; // size of chunk
	*ptr++ = 0x08; // sample precision
	*ptr++ = (uint8)(h >> 8);
	*ptr++ = (uint8)(h); // number of lines (must be a multiple of 8)
	*ptr++ = (uint8)(w >> 8);
	*ptr++ = (uint8)(w); // number of columns (must be a multiple of 8)
	*ptr++ = 0x03; // number of components
	*ptr++ = 0x01; // id of component
	*ptr++ = type ? 0x22 : 0x21; // sampling ratio (h,v)
	*ptr++ = 0x00; // quant table id
	*ptr++ = 0x02; // id of component
	*ptr++ = 0x11; // sampling ratio (h,v)
	*ptr++ = numQtables == 1 ? 0x00 : 0x01; // quant table id
	*ptr++ = 0x03; // id of component
	*ptr++ = 0x11; // sampling ratio (h,v)
	*ptr++ = numQtables == 1 ? 0x00 : 0x01; // quant table id

	mjpeg_create_huffman_header(ptr, lum_dc_codelens, sizeof lum_dc_codelens,
		      lum_dc_symbols, sizeof lum_dc_symbols, 0, 0);
	mjpeg_create_huffman_header(ptr, lum_ac_codelens, sizeof lum_ac_codelens,
		      lum_ac_symbols, sizeof lum_ac_symbols, 0, 1);
	mjpeg_create_huffman_header(ptr, chm_dc_codelens, sizeof chm_dc_codelens,
		      chm_dc_symbols, sizeof chm_dc_symbols, 1, 0);
	mjpeg_create_huffman_header(ptr, chm_ac_codelens, sizeof chm_ac_codelens,
		      chm_ac_symbols, sizeof chm_ac_symbols, 1, 1);

	// MARKER_SOS:
	*ptr++ = 0xFF;  *ptr++ = MARKER_SOS;
	*ptr++ = 0x00; *ptr++ = 0x0C; // size of chunk
	*ptr++ = 0x03; // number of components
	*ptr++ = 0x01; // id of component
	*ptr++ = 0x00; // huffman table id (DC, AC)
	*ptr++ = 0x02; // id of component
	*ptr++ = 0x11; // huffman table id (DC, AC)
	*ptr++ = 0x03; // id of component
	*ptr++ = 0x11; // huffman table id (DC, AC)
	*ptr++ = 0x00; // start of spectral
	*ptr++ = 0x3F; // end of spectral
	*ptr++ = 0x00; // successive approximation bit position (high, low)

	return (int)(ptr - buf);
}


// The default 'luma' and 'chroma' quantizer tables, in zigzag order:
static uint8 const defaultQuantizers[128] = 
{
	// luma table:
	16, 11, 12, 14, 12, 10, 16, 14,
	13, 14, 18, 17, 16, 19, 24, 40,
	26, 24, 22, 22, 24, 49, 35, 37,
	29, 40, 58, 51, 61, 60, 57, 51,
	56, 55, 64, 72, 92, 78, 64, 68,
	87, 69, 55, 56, 80, 109, 81, 87,
	95, 98, 103, 104, 103, 62, 77, 113,
	121, 112, 100, 120, 92, 101, 103, 99,
	// chroma table:
	17, 18, 18, 24, 21, 24, 47, 26,
	26, 47, 99, 66, 56, 66, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99
};

void mjpeg_make_default_qtables(uint8* resultTables, uint32 Q) 
{
	int factor = Q;
	int q;

	if (Q < 1) factor = 1;
	else if (Q > 99) factor = 99;

	if (Q < 50) 
	{
		q = 5000 / factor;
	} 
	else 
	{
		q = 200 - factor*2;
	}

	for (int i = 0; i < 128; ++i) 
	{
		int newVal = (defaultQuantizers[i]*q + 50)/100;
		if (newVal < 1) newVal = 1;
		else if (newVal > 255) newVal = 255;
		resultTables[i] = newVal;
	}
}

BOOL mjpeg_data_rx(MJPEGRXI * p_rxi, uint8 * p_data, int len)
{
	uint8* headerStart = p_data;
	uint32 packetSize = len;
	uint32 resultSpecialHeaderSize = 0;
	
	uint8* qtables = NULL;
	uint32 qtlen = 0;
	uint32 dri = 0;

	// There's at least 8-byte video-specific header
	/*
	0                   1                   2                   3
	0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	| Type-specific |              Fragment Offset                  |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|      Type     |       Q       |     Width     |     Height    |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */

  	resultSpecialHeaderSize = 8;

	uint32 Offset = (uint32)((uint32)headerStart[1] << 16 | (uint32)headerStart[2] << 8 | (uint32)headerStart[3]);
	uint32 Type = (uint32)headerStart[4];
	uint32 type = Type & 1;
	uint32 Q = (uint32)headerStart[5];
	uint32 width = (uint32)headerStart[6] * 8;
	uint32 height = (uint32)headerStart[7] * 8;
	
	if (width == 0) width = 256*8; 		// special case
	if (height == 0) height = 256*8; 	// special case

	if (Type > 63) 
	{
		// Restart Marker header present
		/*
		0                   1                   2                   3
		0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		|       Restart Interval        |F|L|       Restart Count       |
		+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		  */
		if (packetSize < resultSpecialHeaderSize + 4)
		{
			return FALSE;
		}

		uint32 RestartInterval = (uint32)((uint16)headerStart[resultSpecialHeaderSize] << 8 | (uint16)headerStart[resultSpecialHeaderSize + 1]);
		dri = RestartInterval;
		resultSpecialHeaderSize += 4;
	}

	if (Offset == 0) 
	{
		if (Q > 127) 
		{
			// Quantization Table header present
			/*
			0                   1                   2                   3
			0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
			+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			|      MBZ      |   Precision   |             Length            |
			+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			|                    Quantization Table Data                    |
			|                              ...                              |
			+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
			*/
			if (packetSize < resultSpecialHeaderSize + 4) 
			{
				return FALSE;
			}

			uint32 MBZ = (uint32)headerStart[resultSpecialHeaderSize];
			if (MBZ == 0) 
			{
				// uint32 Precision = (uint32)headerStart[resultSpecialHeaderSize + 1];
				uint32 Length = (uint32)((uint16)headerStart[resultSpecialHeaderSize + 2] << 8 | (uint16)headerStart[resultSpecialHeaderSize + 3]);

				//ASSERT(Length == 128);

				resultSpecialHeaderSize += 4;

				if (packetSize < resultSpecialHeaderSize + Length)
				{
					return FALSE;
				}

				qtlen = Length;
				qtables = &headerStart[resultSpecialHeaderSize];

				resultSpecialHeaderSize += Length;
			}
		}
	}

	// If this is the first (or only) fragment of a JPEG frame
	if (Offset == 0) 
	{
		uint8 newQtables[128];
		if (qtlen == 0) 
		{
			// A quantization table was not present in the RTP JPEG header,
			// so use the default tables, scaled according to the "Q" factor:
			mjpeg_make_default_qtables(newQtables, Q);
			qtables = newQtables;
			qtlen = sizeof newQtables;
		}
		
		p_rxi->d_offset = mjpeg_create_header(p_rxi->p_buf, type, width, height, qtables, qtlen, dri);
	}

	if ((p_rxi->d_offset + packetSize - resultSpecialHeaderSize) >= RTP_MAX_VIDEO_BUFF)
	{
		log_print(LOG_ERR, "%s, fragment packet too big %d!!!", __FUNCTION__, p_rxi->d_offset + packetSize - resultSpecialHeaderSize);
		return FALSE;
	}
	
	memcpy(p_rxi->p_buf + p_rxi->d_offset, headerStart + resultSpecialHeaderSize, packetSize - resultSpecialHeaderSize);
	p_rxi->d_offset += packetSize - resultSpecialHeaderSize;
		
	// The RTP "M" (marker) bit indicates the last fragment of a frame:
	if (p_rxi->rtprxi.rxf_marker)
	{
		if (p_rxi->d_offset >= 2 && !(p_rxi->p_buf[p_rxi->d_offset-2] == 0xFF && p_rxi->p_buf[p_rxi->d_offset-1] == MARKER_EOI)) 
		{
    		p_rxi->p_buf[p_rxi->d_offset++] = 0xFF;
    		p_rxi->p_buf[p_rxi->d_offset++] = MARKER_EOI;
  		}

  		if (p_rxi->pkt_func)
  		{
  			p_rxi->pkt_func(p_rxi->p_buf, p_rxi->d_offset, p_rxi->rtprxi.prev_ts, p_rxi->rtprxi.prev_seq, p_rxi->user_data);
  		}

  		p_rxi->d_offset = 0;
	}

	return TRUE;
}

BOOL mjpeg_rtp_rx(MJPEGRXI * p_rxi, uint8 * p_data, int len)
{
	if (p_rxi == NULL)
	{
		return FALSE;
	}
	
	if (!rtp_data_rx(&p_rxi->rtprxi, p_data, len))
	{
		return FALSE;
	}

	return mjpeg_data_rx(p_rxi, p_rxi->rtprxi.p_data, p_rxi->rtprxi.len);
}

BOOL mjpeg_rxi_init(MJPEGRXI * p_rxi, VRTPRXCBF cbf, void * p_userdata)
{
	memset(p_rxi, 0, sizeof(MJPEGRXI));

	p_rxi->buf_len = RTP_MAX_VIDEO_BUFF;
	
	p_rxi->p_buf_org = (uint8 *)malloc(p_rxi->buf_len);
	if (p_rxi->p_buf_org == NULL)
	{
		return FALSE;
    }
    
	p_rxi->p_buf = p_rxi->p_buf_org + 32;
	p_rxi->buf_len -= 32;
	p_rxi->pkt_func = cbf;
    p_rxi->user_data = p_userdata;

	return TRUE;
}

void mjpeg_rxi_deinit(MJPEGRXI * p_rxi)
{
	if (p_rxi->p_buf_org)
	{
		free(p_rxi->p_buf_org);
    }
    
	memset(p_rxi, 0, sizeof(MJPEGRXI));
}




