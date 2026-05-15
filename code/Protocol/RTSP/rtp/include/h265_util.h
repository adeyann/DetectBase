
#ifndef H265_UTIL_H
#define H265_UTIL_H

typedef struct
{
	int		nal_len;
	uint32	general_profile_idc;
	uint32	general_level_idc;
	uint32	separate_colour_plane_flag;
	uint32	pic_width_in_luma_samples;
	uint32	pic_height_in_luma_samples;
	uint32	sps_max_sub_layers_minus1;
	uint32	sps_seq_parameter_set_id;
	uint32	chroma_format_idc;
	uint32	conformance_window_flag;
	uint32	conf_win_left_offset;
	uint32	conf_win_right_offset;
	uint32	conf_win_top_offset;
	uint32	conf_win_bottom_offset;	
	uint32	bit_depth_luma_minus8;
	uint32	bit_depth_chroma_minus8;
} h265_t;

#ifdef __cplusplus
extern "C" {
#endif

void h265_parser_init(h265_t * h);
int  h265_parser_parse(h265_t * h, uint8 * p_data, int len);

#ifdef __cplusplus
}
#endif

#endif // H265_UTIL_H



