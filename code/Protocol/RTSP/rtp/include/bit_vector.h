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

#ifndef BIT_VECTOR_HH
#define BIT_VECTOR_HH

#include "sys_inc.h"

class BitVector 
{
public:
  BitVector(uint8 * baseBytePtr, uint32 baseBitOffset, uint32 totNumBits);

  void      setup(uint8 * baseBytePtr, uint32 baseBitOffset, uint32 totNumBits);
  void      putBits(uint32 from, uint32 numBits); // "numBits" <= 32
  void      put1Bit(uint32 bit);
  uint32    getBits(uint32 numBits); // "numBits" <= 32
  uint32    get1Bit();
  BOOL      get1BitBoolean() { return get1Bit() != 0; }
  void      skipBits(uint32 numBits);
  uint32    curBitIndex() const { return fCurBitIndex; }
  uint32    totNumBits() const { return fTotNumBits; }
  uint32    numBitsRemaining() const { return fTotNumBits - fCurBitIndex; }
  uint32    get_expGolomb();
      // Returns the value of the next bits, assuming that they were encoded using an exponential-Golomb code of order 0

private:
  uint8 *   fBaseBytePtr;
  uint32    fBaseBitOffset;
  uint32    fTotNumBits;
  uint32    fCurBitIndex;
};

// A general bit copy operation:
void shiftBits(uint8 * toBasePtr, uint32 toBitOffset, uint8 const* fromBasePtr, uint32 fromBitOffset, uint32 numBits);

#endif



