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

#include "media_util.h"


uint32 remove_emulation_bytes(uint8* to, uint32 toMaxSize, uint8* from, uint32 fromSize) 
{
	uint32 toSize = 0;
	uint32 i = 0;
	
	while (i < fromSize && toSize+1 < toMaxSize) 
	{
		if (i+2 < fromSize && from[i] == 0 && from[i+1] == 0 && from[i+2] == 3) 
		{
			to[toSize] = to[toSize+1] = 0;
			toSize += 2;
			i += 3;
		}
		else 
		{
			to[toSize] = from[i];
			toSize += 1;
			i += 1;
		}
	}

	return toSize;
}

static uint8 * avc_find_startcode_internal(uint8 *p, uint8 *end)
{
    uint8 *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) 
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
        	return p;
		}          
    }

    for (end -= 3; p < end; p += 4) 
    {
        uint32 x = *(const uint32*)p;
        
        if ((x - 0x01010101) & (~x) & 0x80808080) 
        { // generic
            if (p[1] == 0) 
            {
                if (p[0] == 0 && p[2] == 1)
                {
                    return p;
                }
                
                if (p[2] == 0 && p[3] == 1)
                {
                    return p+1;
                }    
            }
            
            if (p[3] == 0) 
            {
                if (p[2] == 0 && p[4] == 1)
                {
                    return p+2;
                }
                
                if (p[4] == 0 && p[5] == 1)
                {
                    return p+3;
                }    
            }
        }
    }

    for (end += 3; p < end; p++) 
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
        	return p;
        }
    }

    return end + 3;
}

uint8 * avc_find_startcode(uint8 *p, uint8 *end)
{
    uint8 *out = avc_find_startcode_internal(p, end);
    if (p<out && out<end && !out[-1])
    {
    	out--;
    }
    
    return out;
}

uint8 * avc_split_nalu(uint8 * e_buf, int e_len, int * s_len, int * d_len)
{
	int e_i = 4;
	//int nalu_len = 0;

	*d_len = 0;
	uint32 tmp_w;
	uint32 split_w1 = 0x01000000;
	//uint32 split_w2 = 0x00010000;

	memcpy(&tmp_w, e_buf, 4);

	if (tmp_w == split_w1)
	{
		*s_len = 4;
	}
	else
	{
		return NULL;
	}
	
	// Find the next start code or the end of the file
	while (e_i < e_len)
	{
		if (e_i >= (e_len-4))
		{
			e_i = e_len;
			break;
		}

		memcpy(&tmp_w, e_buf+e_i, 4);
		
		if (tmp_w == split_w1)
		{
			break;
		}

		e_i++;
	}

	*d_len = e_i;

	if (e_i >= e_len)
	{
		return NULL;
	}
	
	return (e_buf+e_i);	// Starting position of next data frame 
}


namespace
{
	class __GET_TICK_COUNT
	{
	public:
		__GET_TICK_COUNT()
		{
			if (gettimeofday(&tv_, NULL) != 0)
				throw 0;
		}
		timeval tv_;
	};
	__GET_TICK_COUNT timeStart;
}


unsigned long gettickcount()
{
	static time_t	secStart = timeStart.tv_.tv_sec;
	static time_t	usecStart = timeStart.tv_.tv_usec;
	timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec - secStart) * 1000 + (tv.tv_usec - usecStart) / 1000;
}

unsigned long gettickcount2()
{
	timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}