#pragma once

#include "h264.h"

#include <stdint.h>
#include <cstddef>
#include <string>

// H.264 SPS 파싱 결과 저장 구조체
typedef struct H264SPSInfo_t
{
	bool valid       = false; // 파싱 성공 및 유효 데이터 여부
	int  profile_idc = 0;
	int  level_idc   = 0;
	int  width       = 0;
	int  height      = 0;
	// 필요한 다른 정보 추가 가능 (예: framerate)

	// 비교 연산자 오버로딩
	bool operator==( const struct H264SPSInfo_t& other ) const
	{
		return
			valid       && other.valid       &&
			profile_idc == other.profile_idc &&
			level_idc   == other.level_idc   &&
			width       == other.width       &&
			height      == other.height;
		// 다른 멤버 변수 추가 시 비교 로직에도 추가
	}

	bool operator!=( const struct H264SPSInfo_t& other ) const
	{
		return !( *this == other );
	}
} H264SPSInfo;

// H.264 PPS 파싱 결과 저장 구조체
typedef struct H264PPSInfo_t
{
	bool valid = false; // 파싱 성공 및 유효 데이터 여부
	bool entropy_coding_mode_flag = false; // 0: CAVLC, 1: CABAC
	// 필요한 다른 정보 추가 가능

	// 비교 연산자 오버로딩
	bool operator==( const struct H264PPSInfo_t& other ) const
	{
		return
			valid && other.valid &&
			entropy_coding_mode_flag == other.entropy_coding_mode_flag;
		// 다른 멤버 변수 추가 시 비교 로직에도 추가
	}

	bool operator!=( const struct H264PPSInfo_t& other ) const
	{
		return !( *this == other );
	}
} H264PPSInfo;

// 간단한 비트스트림 리더 헬퍼 클래스
class BitReader
{
private:
	// Exp-Golomb 코드를 디코딩하는 내부 함수
	unsigned int read_exp_golomb_code( void );

	// 1 비트 읽기 (내부용, 경계 체크 없음)
	unsigned int read_bit_internal( void );

public:
	BitReader( const uint8_t* data, size_t size_bytes );

	// n 비트 읽기
	unsigned int read_bits( const int n );

	// Unsigned Exp-Golomb (ue(v)) 읽기
	unsigned int read_ue( void );

	// Signed Exp-Golomb (se(v)) 읽기
	int read_se();

	// 현재 위치 건너뛰기
	void skip_bits( const int n );

	// 현재 비트 위치 반환
	size_t get_current_bit() const { return current_bit_; }
	bool   is_eof()          const { return current_bit_ >= size_bits_; }

private:
	const uint8_t* data_;
	size_t         size_bits_;
	size_t         current_bit_;
};

bool parse_sps( const uint8_t* data, int size, H264SPSInfo& info );
bool parse_pps( const uint8_t* data, int size, H264PPSInfo& info );

// 파싱된 정보를 문자열로 변환하는 함수 (로그용)
std::string sps_info_to_string( const H264SPSInfo& info );
std::string pps_info_to_string( const H264PPSInfo& info );