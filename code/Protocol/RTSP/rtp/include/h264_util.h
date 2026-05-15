
#ifndef H264_UTIL_H
#define H264_UTIL_H


typedef struct
{
	int		i_ref_idc;  // nal_priority_e
	int		i_type;     // nal_unit_type_e

	/* This data are raw payload */
	int		i_payload;
	uint8 *	p_payload;
} nal_t;

typedef struct
{
    int 	i_width;
    int 	i_height; 
    int 	i_nal_type;
    int 	i_ref_idc;
    int 	i_idr_pic_id;
    int 	i_frame_num;
    int 	i_poc;
    int 	b_key;
    int 	i_log2_max_frame_num;
    int 	i_poc_type;
    int 	i_log2_max_poc_lsb; 
} h264_t;


#ifdef __cplusplus
extern "C" {
#endif

void h264_parser_init(h264_t * h);
void h264_parser_parse(h264_t * h, nal_t * nal, int * pb_nal_start);

#ifdef __cplusplus
}
#endif

#endif // H264_UTIL_H



