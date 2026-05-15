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
#include "bit_vector.h"

BitVector::BitVector(uint8* baseBytePtr, uint32 baseBitOffset, uint32 totNumBits) 
{
	setup(baseBytePtr, baseBitOffset, totNumBits);
}

void BitVector::setup(uint8* baseBytePtr, uint32 baseBitOffset, uint32 totNumBits) 
{
	fBaseBytePtr = baseBytePtr;
	fBaseBitOffset = baseBitOffset;
	fTotNumBits = totNumBits;
	fCurBitIndex = 0;
}

static uint8 const singleBitMask[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

#define MAX_LENGTH 32

void BitVector::putBits(uint32 from, uint32 numBits) 
{
	if (numBits == 0)
	{
		return; 
	}
	
	uint8 tmpBuf[4];
	uint32 overflowingBits = 0;

	if (numBits > MAX_LENGTH)
	{
		numBits = MAX_LENGTH;
	}

	if (numBits > fTotNumBits - fCurBitIndex) 
	{
		overflowingBits = numBits - (fTotNumBits - fCurBitIndex);
	}

	tmpBuf[0] = (uint8)(from>>24);
	tmpBuf[1] = (uint8)(from>>16);
	tmpBuf[2] = (uint8)(from>>8);
	tmpBuf[3] = (uint8)from;

	shiftBits(fBaseBytePtr, fBaseBitOffset + fCurBitIndex, /* to */
		tmpBuf, MAX_LENGTH - numBits, /* from */
		numBits - overflowingBits /* num bits */);
	fCurBitIndex += numBits - overflowingBits;
}

void BitVector::put1Bit(uint32 bit) 
{
	// The following is equivalent to "putBits(..., 1)", except faster:
	if (fCurBitIndex >= fTotNumBits) 
	{ 
		/* overflow */
		return;
	} 
	else 
	{
		uint32 totBitOffset = fBaseBitOffset + fCurBitIndex++;
		uint8 mask = singleBitMask[totBitOffset%8];

		if (bit) 
		{
			fBaseBytePtr[totBitOffset/8] |= mask;
		} 
		else 
		{
			fBaseBytePtr[totBitOffset/8] &=~ mask;
		}
	}
}

uint32 BitVector::getBits(uint32 numBits) 
{
	if (numBits == 0)
	{
		return 0;
	}
	
	uint8 tmpBuf[4];
	uint32 overflowingBits = 0;

	if (numBits > MAX_LENGTH) 
	{
		numBits = MAX_LENGTH;
	}

	if (numBits > fTotNumBits - fCurBitIndex) 
	{
		overflowingBits = numBits - (fTotNumBits - fCurBitIndex);
	}

	shiftBits(tmpBuf, 0, /* to */
		fBaseBytePtr, fBaseBitOffset + fCurBitIndex, /* from */
		numBits - overflowingBits /* num bits */);
	fCurBitIndex += numBits - overflowingBits;

	uint32 result = (tmpBuf[0]<<24) | (tmpBuf[1]<<16) | (tmpBuf[2]<<8) | tmpBuf[3];
	result >>= (MAX_LENGTH - numBits); 			// move into low-order part of word
	result &= (0xFFFFFFFF << overflowingBits); 	// so any overflow bits are 0
	return result;
}

uint32 BitVector::get1Bit() 
{
  // The following is equivalent to "getBits(1)", except faster:

	if (fCurBitIndex >= fTotNumBits) 
	{ 
		/* overflow */
		return 0;
	} 
	else 
	{
		uint32 totBitOffset = fBaseBitOffset + fCurBitIndex++;
		uint8 curFromByte = fBaseBytePtr[totBitOffset/8];
		uint32 result = (curFromByte >> (7-(totBitOffset%8))) & 0x01;
		return result;
	}
}

void BitVector::skipBits(uint32 numBits)
{
	if (numBits > fTotNumBits - fCurBitIndex) 
	{ 
		/* overflow */
		fCurBitIndex = fTotNumBits;
	} 
	else
	{
		fCurBitIndex += numBits;
	}
}

uint32 BitVector::get_expGolomb() 
{
	uint32 numLeadingZeroBits = 0;
	uint32 codeStart = 1;

	while (get1Bit() == 0 && fCurBitIndex < fTotNumBits) 
	{
		++numLeadingZeroBits;
		codeStart *= 2;
	}

	return codeStart - 1 + getBits(numLeadingZeroBits);
}

void shiftBits(uint8 * toBasePtr, uint32 toBitOffset, uint8 const * fromBasePtr, uint32 fromBitOffset, uint32 numBits) 
{
	if (numBits == 0)
	{
		return;
	}
	
	/* Note that from and to may overlap, if from>to */
	uint8 const* fromBytePtr = fromBasePtr + fromBitOffset/8;
	uint32 fromBitRem = fromBitOffset%8;
	uint8* toBytePtr = toBasePtr + toBitOffset/8;
	uint32 toBitRem = toBitOffset%8;

	while (numBits-- > 0) 
	{
		uint8 fromBitMask = singleBitMask[fromBitRem];
		uint8 fromBit = (*fromBytePtr)&fromBitMask;
		uint8 toBitMask = singleBitMask[toBitRem];

		if (fromBit != 0) 
		{
			*toBytePtr |= toBitMask;
		}
		else 
		{
			*toBytePtr &=~ toBitMask;
		}

		if (++fromBitRem == 8)
		{
			++fromBytePtr;
			fromBitRem = 0;
		}
		
		if (++toBitRem == 8) 
		{
			++toBytePtr;
			toBitRem = 0;
		}
	}
}



