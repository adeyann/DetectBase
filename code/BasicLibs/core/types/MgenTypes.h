/**
 * @file MgenTypes.h
 * @brief MGEN 프로젝트 전반에서 사용하는 기초 데이터 타입 및 공통 상수를 정의합니다.
 */

#ifndef _MGEN_TYPES_H_
#define _MGEN_TYPES_H_

#pragma once

#include <chrono> // std::chrono 라이브러리 활용

/* --------------------------------------------------------------------------------------------------- */
#if defined( _WIN32 ) || defined( _WIN64 )
  #define __WINDOWS_OS__  1
  #define __VXWORKS_OS__  0
  #define __LINUX_OS__    0
#else
  /**
   * @note MGEN 서비스의 주 타겟 운영체제는 Linux입니다.
   */
  #define __WINDOWS_OS__  0
  #define __VXWORKS_OS__  0
  #define __LINUX_OS__    1
#endif

/* --------------------------------------------------------------------------------------------------- */
using uint8  = unsigned char;  //  8bit = 1byte
using uint16 = unsigned short; // 16bit = 2byte
using uint32 = unsigned int;   // 32bit = 4byte
using int32  = int;            // 32bit = 4byte

/* --------------------------------------------------------------------------------------------------- */
namespace MGEN::Type
{
    /**
     * @brief 프로젝트 내 객체 식별을 위한 기본 정수형 타입들을 정의합니다.
     */
    using UnitID      = int;
    using DeviceID    = int;
    using CameraID    = MGEN::Type::DeviceID; // RTSP Proxy Camera ID
}

namespace MGEN
{
    /**
     * @enum CameraColorMode
     * @brief 카메라의 영상 출력 모드를 정의합니다.
     */
    enum class CameraColorMode {
        RGB,     // 주간 모드 (컬러)
        IR,      // 야간 모드 (적외선/흑백)
        UNKNOWN  // 판별 불가 상태
    };

    /**
     * @brief ID 미설정 및 해시 관련 공통 상수입니다.
     */
    constexpr MGEN::Type::UnitID UNIT_ID_NOT_SET = -1;
    constexpr unsigned int       HASH_MAGIC_NUMB = 0x9e3779b9;
}

namespace MGEN
{
    /**
     * @brief 시간 관련 타입 앨리어스 정의입니다.
     */
    using TimePoint  = std::chrono::steady_clock::time_point;
    using DetectTime = std::chrono::time_point<std::chrono::system_clock>;
    using EventTime  = DetectTime;
}

/* --------------------------------------------------------------------------------------------------- */
#define DEFINE     constexpr auto
#define UNUSED( x ) ( void )( x )

#ifndef BOOL_DEFFINES_
#define BOOL_DEFFINES_
    /**
     * @brief 레거시 코드와의 호환성을 위한 정의입니다.
     */
    using   BOOL = int;
    #define TRUE  1
    #define FALSE 0
#endif

#endif // _MGEN_TYPES_H_