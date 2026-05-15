#include "h264_parser.h"
#include "MgenLogger.h"

#include <sstream>
#include <iomanip> // std::setw를 사용하기 위해 필요

BitReader::BitReader( const uint8_t* data, size_t size )
	: data_       ( data )
	, size_bits_  ( size * 8 )
	, current_bit_( 0)
{
	//
}

unsigned int BitReader::read_bits( const int n )
{
	unsigned int value = 0;
	if( current_bit_ + n > size_bits_ )
		return 0; // 데이터 부족

	for( int i = 0; i < n; ++i )
	{
		value = (value << 1) | read_bit_internal();
	}

	return value;
}

unsigned int BitReader::read_exp_golomb_code( void )
{
	int leading_zeros = 0;
	while( current_bit_ < size_bits_ && read_bit_internal() == 0 )
	{
		leading_zeros++;
	}
	if( current_bit_ + leading_zeros > size_bits_ )
		return 0; // 데이터 부족

	unsigned int code_num = 0;
	for( int i = 0; i < leading_zeros; ++i )
	{
		unsigned int bit = read_bit_internal();
		code_num |= (bit << (leading_zeros - 1 - i));
	}

	return (1 << leading_zeros) - 1 + code_num;
}

unsigned int BitReader::read_bit_internal( void )
{
	size_t byte_index = current_bit_ / 8;
	size_t bit_offset = 7 - (current_bit_ % 8);
	current_bit_++;

	return (data_[byte_index] >> bit_offset) & 1;
}

unsigned int BitReader::read_ue()
{
	return read_exp_golomb_code();
}

int BitReader::read_se()
{
	unsigned int code_num = read_exp_golomb_code();
	if( code_num % 2 == 0 )
	{ // even
		return - (int)(code_num / 2);
	}
	else
	{ // odd
		return (int)((code_num + 1) / 2);
	}
}

void BitReader::skip_bits( const int n )
{
	if( current_bit_ + n <= size_bits_ )
	{
		current_bit_ += n;
	}
	else
	{
		current_bit_ = size_bits_; // 버퍼 끝까지만 이동
	}
}

// H.264 SPS 파서 함수 (주요 정보만 추출)
bool parse_sps( const uint8_t* data, int size, H264SPSInfo& info )
{
	if (!data || size < 4)
	{
		return false;
	}

	BitReader br( data, size );

	// H.264 spec 7.3.2.1.1:
	// chroma_format_idc는 High Profile이 아닌 경우 1 (YUV 4:2:0)로 간주됨.
	unsigned int chroma_format_idc = 1;

	try
	{
		// NAL header (forbidden_zero_bit + nal_ref_idc + nal_unit_type)
		br.skip_bits(8);
		info.profile_idc = br.read_bits(8);

		// constraint_set flags + reserved
		br.skip_bits(8);
		info.level_idc = br.read_bits(8);

		// seq_parameter_set_id
		br.read_ue();

		// High Profile (또는 그 변종)인 경우 chroma_format_idc 등을 추가로 파싱
		if( info.profile_idc == 100 || info.profile_idc == 110 || info.profile_idc == 122 ||
			info.profile_idc == 244 || info.profile_idc ==  44 || info.profile_idc ==  83 ||
			info.profile_idc ==  86 || info.profile_idc == 118 || info.profile_idc == 128 ||
			info.profile_idc == 138 || info.profile_idc == 139 || info.profile_idc == 134 )
		{
			chroma_format_idc = br.read_ue();

			// separate_colour_plane_flag
			if (chroma_format_idc == 3)
			{
				br.skip_bits(1);
			}

			br.read_ue();    // bit_depth_luma_minus8
			br.read_ue();    // bit_depth_chroma_minus8
			br.skip_bits(1); // qpprime_y_zero_transform_bypass_flag

			unsigned int seq_scaling_matrix_present_flag = br.read_bits(1);
			if( seq_scaling_matrix_present_flag )
			{
				// 복잡한 스케일링 매트릭스 파싱 생략
				// MLOG_WARN("SPS scaling matrix parsing not implemented, skipping.");
				// 파싱 실패 대신, 스케일링 리스트를 건너뛰도록 수정 (더 견고함)
				for( int i = 0; i < ((chroma_format_idc != 3) ? 8 : 12); i++ )
				{
					if( br.read_bits(1) ) // seq_scaling_list_present_flag[i]
					{
						if( i < 6 ) // 4x4
						{
							// skip_scaling_list(16)
							for( int j = 0; j < 16; j++ ) br.read_se();
						}
						else // 8x8
						{
							// skip_scaling_list(64)
							for( int j = 0; j < 64; j++ ) br.read_se();
						}
					}
				}
				// return false; // 스케일링 매트릭스가 있어도 계속 파싱 진행
			}
		}

		// log2_max_frame_num_minus4
		br.read_ue();
		unsigned int pic_order_cnt_type = br.read_ue();

		if( pic_order_cnt_type == 0 )
		{
			// log2_max_pic_order_cnt_lsb_minus4
			br.read_ue();
		}
		else if (pic_order_cnt_type == 1)
		{
			br.skip_bits(1); // delta_pic_order_always_zero_flag
			br.read_se();    // offset_for_non_ref_pic
			br.read_se();    // offset_for_top_to_bottom_field

			unsigned int num_ref_frames_in_pic_order_cnt_cycle = br.read_ue();
			for( unsigned int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; ++i )
			{
				br.read_se(); // offset_for_ref_frame[i]
			}
		}

		// num_ref_frames
		br.read_ue();
		// gaps_in_frame_num_value_allowed_flag
		br.skip_bits(1);

		unsigned int pic_width_in_mbs_minus1 = br.read_ue();
		unsigned int pic_height_in_map_units_minus1 = br.read_ue();
		unsigned int frame_mbs_only_flag = br.read_bits(1);

		info.width = (pic_width_in_mbs_minus1 + 1) * 16;
		// (2 - frame_mbs_only_flag)는 프레임(1) 또는 필드(2) 인코딩 여부
		info.height = (pic_height_in_map_units_minus1 + 1) * 16 * (2 - frame_mbs_only_flag);

		if( !frame_mbs_only_flag )
		{
			br.skip_bits(1); // mb_adaptive_frame_field_flag
		}

		// direct_8x8_inference_flag
		br.skip_bits(1);

		unsigned int frame_cropping_flag = br.read_bits(1);
		if( frame_cropping_flag )
		{
			unsigned int crop_left = br.read_ue();
			unsigned int crop_right = br.read_ue();
			unsigned int crop_top = br.read_ue();
			unsigned int crop_bottom = br.read_ue();

			// H.264 Spec (Table 6-1)
			unsigned int SubWidthC = 1, SubHeightC = 1;
			if (chroma_format_idc == 1) // YUV 4:2:0
			{
				SubWidthC  = 2;
				SubHeightC = 2;
			}
			else if (chroma_format_idc == 2) // YUV 4:2:2
			{
				SubWidthC  = 2;
				SubHeightC = 1;
			}
			// chroma_format_idc == 3 (YUV 4:4:4) 또는 0 (Monochrome)은 SubWidthC=1, SubHeightC=1

			int crop_unit_x = (chroma_format_idc  == 0) ? 1 : SubWidthC;
			int crop_unit_y = ((chroma_format_idc == 0) ? 1 : SubHeightC) * (2 - frame_mbs_only_flag);

			info.width  -= (crop_left + crop_right) * crop_unit_x;
			info.height -= (crop_top + crop_bottom) * crop_unit_y;
		}

		// VUI 파라미터 등 이후 데이터는 생략

		info.valid = true; // 파싱 성공
		return true;
	}
	catch (...) // BitReader가 버퍼 끝을 넘어가면 예외 발생 가능 (구현에 따라 다름)
	{
		MLOG_WARN("CAM<%d> Exception during SPS parsing (data size: %d)", -1, size); // cam_id를 알 수 없으므로 -1
		info.valid = false;
		return false;
	}
}

// H.264 PPS 파서 함수 (주요 정보만 추출)
bool parse_pps( const uint8_t* data, int size, H264PPSInfo& info )
{
	if( !data || size < 1 )
		return false;

	BitReader br( data, size );

	try {
		// NAL header
		br.skip_bits(8);

		br.read_ue(); // pic_parameter_set_id
		br.read_ue(); // seq_parameter_set_id
		info.entropy_coding_mode_flag = (br.read_bits(1) == 1);

		// 이후 데이터 파싱 생략 (필요 시 추가)
		return true;
	}
	catch (...) {
		return false;
	}
}

// 파싱된 정보를 문자열로 변환하는 함수 (로그용)
std::string sps_info_to_string( const H264SPSInfo& info ) {
	std::stringstream ss;

	// 각 숫자 변수 앞에 std::setw(4)를 배치합니다.
    ss << "Profile: " << std::setw(4) << info.profile_idc
       << ", Level: " << std::setw(4) << info.level_idc
       << ", Resolution: " << std::setw(4) << info.width << "x" << std::setw(4) << info.height;

	return ss.str();
}

std::string pps_info_to_string( const H264PPSInfo& info ) {
	std::stringstream ss;
	ss << "Entropy Coding: " << (info.entropy_coding_mode_flag ? "CABAC" : "CAVLC");
	return ss.str();
}