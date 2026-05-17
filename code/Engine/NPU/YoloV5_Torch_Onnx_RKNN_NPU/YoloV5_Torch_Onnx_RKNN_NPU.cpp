#include "YoloV5_Torch_Onnx_RKNN_NPU.h"
#include "SafeThread.h"
#include "MgenLogger.h"
#include "file_utils.h"
#include "MgenTypes.h"

// C++ : STL
#include <chrono>
#include <fstream>
#include <vector>
#include <numeric>      // std::accumulate
#include <algorithm>    // std::max, std::min, std::sort
#include <stdexcept>
#include <map>
#include <cmath>        // std::ceil, floorf
#include <sstream>      // ostringstream
#include <utility> // std::swap을 사용하기 위해 필요

const int anchor_0[6] = { 10,  13, 16,  30,  33,  23  }; // stride 8
const int anchor_1[6] = { 30,  61, 62,  45,  59,  119 }; // stride 16
const int anchor_2[6] = { 116, 90, 156, 198, 373, 326 }; // stride 32

namespace MGEN
{
    YoloV5_Torch_Onnx_RKNN_NPU::YoloV5_Torch_Onnx_RKNN_NPU( const EngineProfile& profile, const InferDeviceID device_id ) noexcept
        : EngineHandlerBase( profile, device_id )
        , handler_name_                  ( std::string{ "YoloV5_Torch_Onnx_RKNN_NPU" } )
        , confidence_threshold_          ( profile.GetConfidenceThreshold() )
        , nms_threshold_                 ( profile.GetNmsThreshold() )
    {
        MLOG_DEBUG( "%s[%d] created for profile '%s' (ID: %d) with batch size %u.",
            this->handler_name_.c_str(), this->device_id_, profile_.GetProfileName().c_str(), profile_.GetProfileUUID(), this->batch_size_ );
    }

    bool YoloV5_Torch_Onnx_RKNN_NPU::InitializeDevice()
    {
        return true;
    }

    cv::Rect YoloV5_Torch_Onnx_RKNN_NPU::GetRect( cv::Mat& img, float bbox[4] )
    {
        float l, r, t, b;
        float r_w = YOLOV5_RKNN::INPUT_W / (img.cols * 1.0);
        float r_h = YOLOV5_RKNN::INPUT_H / (img.rows * 1.0);

        if( r_h > r_w )
        {
            l = bbox[0] - bbox[2] / 2.f;
            r = bbox[0] + bbox[2] / 2.f;
            t = bbox[1] - bbox[3] / 2.f - (YOLOV5_RKNN::INPUT_H - r_w * img.rows) / 2;
            b = bbox[1] + bbox[3] / 2.f - (YOLOV5_RKNN::INPUT_H - r_w * img.rows) / 2;
            l = l / r_w;
            r = r / r_w;
            t = t / r_w;
            b = b / r_w;
        }
        else
        {
            l = bbox[0] - bbox[2] / 2.f - (YOLOV5_RKNN::INPUT_W - r_h * img.cols) / 2;
            r = bbox[0] + bbox[2] / 2.f - (YOLOV5_RKNN::INPUT_W - r_h * img.cols) / 2;
            t = bbox[1] - bbox[3] / 2.f;
            b = bbox[1] + bbox[3] / 2.f;
            l = l / r_h;
            r = r / r_h;
            t = t / r_h;
            b = b / r_h;
        }

        return cv::Rect(round(l), round(t), round(r - l), round(b - t));
    }

    static const float CalculateOverlap( float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1 )
    {
        float  w = fmaxf(0.f, fminf(xmax0, xmax1) - fmaxf(xmin0, xmin1) + 1.0f);
        float  h = fmaxf(0.f, fminf(ymax0, ymax1) - fmaxf(ymin0, ymin1) + 1.0f);
        float  i = w * h;
        float  u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) + (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
        return u <= 0.f ? 0.f : (i / u);
    }

    static const int nms( int validCount, std::vector<float>& outputLocations, const std::vector<int>& classIds, std::vector<int>& order, int filterId, float threshold )
    {
        for( int i = 0; i < validCount; ++i )
        {
            int n = order[i];
            if( n == -1 || classIds[n] != filterId )
                continue;

            for( int j = i + 1; j < validCount; ++j )
            {
                int m = order[j];
                if( m == -1 || classIds[m] != filterId )
                    continue;

                float xmin0 = outputLocations[n * 4 + 0];
                float ymin0 = outputLocations[n * 4 + 1];
                float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
                float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

                float xmin1 = outputLocations[m * 4 + 0];
                float ymin1 = outputLocations[m * 4 + 1];
                float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
                float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

                // IOU 너무 높은(너무 겹치는) 애들 Skip
                float iou = CalculateOverlap( xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1 );
                if( iou > threshold ) {
                    order[j] = -1;
                }
            }
        }
        return 0;
    }

    // input(confidence) 을 내림차순 정렬하고 indices 도 같은 순서로 재배치한다.
    // 기존 재귀 quicksort 를 std::sort + pair 변환 방식으로 교체 (재귀 금지 표준 준수).
    static void sort_by_confidence_desc( std::vector<float>& input, int left, int right, std::vector<int>& indices )
    {
        if( left >= right ) {
            return;
        }

        std::vector<std::pair<float, int>> pairs;
        const int span = right - left + 1;
        pairs.reserve( static_cast<size_t>( span ) );
        for( int i = left; i <= right; ++i ){
            pairs.emplace_back( input[i], indices[i] );
        }

        std::sort( pairs.begin(), pairs.end(), []( const auto& a, const auto& b ){
            return a.first > b.first; // 내림차순
        });

        for( int i = 0; i < span; ++i ){
            input  [left + i] = pairs[i].first;
            indices[left + i] = pairs[i].second;
        }
    }

    inline static float clip_value( float val, float min, float max )
    {
        return val <= min ? min : ( val >= max ? max : val );
    }

    /* [Analysis]
    아핀 변환(Affine transformation)
    두 아핀 공간 U, V간 공선점(Collinear point)을 유지하는 동형사상
    여기서 동형사상은 전단사인 선형사상을 말하며, 아핀공간은 원점이 존재하지 않는 벡터공간
    점, 직선, 평면을 보존하는 선형 매핑 방식(정확히는 비선형이지만...)
    주어진 입력벡터 X와 출력벡터 Y에 대하여 아핀 변환 f:U->V는 아래와 같이 나타난다.

        Y = f(X) = WX + b

    선형변환 이후 시프팅이 일어나기 때문에, 기본적으로 비선형 변환임.
    하지만 비선형 구조가 시프팅에 그치기 떄문에 basis가 이루는 격자구조는 여전히 모든 영역에서 동일하게 유지됨.
    비선형성을 가지게 하기 위해서는 아핀 변환이 아닌 sigmoid나 ReLU 같은 방식을 사용해야 한다.

    아래 양자화 과정에서 아핀 변환을 쓰는 것은, float_32 값을 int_8 값으로 축약하면서도
    점, 직선, 평면, 평행 성질들의 아핀 공간의 기존값을 유지하기 위함임.

    -----------------------------------------------
    zp    : zero point 값을 unsigned가 아닌 signed에 맞추기 위해 빼주는 값(샘플 코드에서는 -128)
    scale : 샘플 코드에서 0.03 이런 숫자 ( 1 / 256 = 0.0039 )
    */
    static int8_t qnt_f32_to_affine( float f32, int32_t zp, float scale )
    {
        float  dst_val = ( f32 / scale ) + zp;
        int8_t res = static_cast<int8_t>( clip_value( dst_val, -128.0f, 127.0f ) );
        return res;
    }

    /* [Analysis]
    양자화된 정수값( int_8 : -128~127 )을 다시 float으로 만드는 함수
    이 때 zp와 scale이 정상 값으로 세팅되어 있다면 0 <= float <= 1
    */
    static float deqnt_affine_to_f32( int8_t qnt, int32_t zp, float scale )
    {
        return ( static_cast<float>( qnt ) - static_cast<float>( zp ) ) * scale;
    }

    static int rknn_process_impl
    (
        int8_t* input, const int* anchor, int grid_h, int grid_w, int height, int width, int stride,
        std::vector<float>& boxes, std::vector<float>& objProbs, std::vector<int>& classId, float threshold,
        int32_t zp, float scale, size_t class_num
    )
    {
        int    validCount = 0;
        int    grid_len   = grid_h * grid_w;
        int8_t thres_i8   = qnt_f32_to_affine(threshold, zp, scale);

        const size_t base_offset = class_num + YOLOV5_RKNN::BBOX_ELEMENT_COUNT + YOLOV5_RKNN::CONF_ELEMENT_COUNT;

        for( int a = 0; a < 3; a++ ) /*anchor*/
        {
            for( int i = 0; i < grid_h; i++ )
            {
                for( int j = 0; j < grid_w; j++ )
                {
                    int8_t box_confidence = input[ (base_offset * a + 4) * grid_len + i * grid_w + j ];

                    if( box_confidence >= thres_i8 )
                    {
                        int     offset = ( base_offset * a ) * grid_len + i * grid_w + j;
                        int8_t* in_ptr = input + offset;

                        float box_x = (deqnt_affine_to_f32( *in_ptr,              zp, scale )) * 2.0 - 0.5;
                        float box_y = (deqnt_affine_to_f32( in_ptr[grid_len],     zp, scale )) * 2.0 - 0.5;
                        float box_w = (deqnt_affine_to_f32( in_ptr[2 * grid_len], zp, scale )) * 2.0;
                        float box_h = (deqnt_affine_to_f32( in_ptr[3 * grid_len], zp, scale )) * 2.0;

                        box_x = (box_x + j) * static_cast<float>( stride );
                        box_y = (box_y + i) * static_cast<float>( stride );
                        box_w = box_w * box_w * static_cast<float>( anchor[a * 2] );
                        box_h = box_h * box_h * static_cast<float>( anchor[a * 2 + 1] );

                        box_x -= (box_w / 2.0);
                        box_y -= (box_h / 2.0);

                        // k=0 은 maxClassProbs / maxClassId 의 초깃값과 동일 → 자기 비교 redundant.
                        // k=1 부터 비교.
                        int8_t maxClassProbs = in_ptr[5 * grid_len];
                        int    maxClassId    = 0;

                        for( size_t k = 1; k < class_num; ++k )
                        {
                            int8_t prob = in_ptr[(5 + k) * grid_len];
                            if( prob > maxClassProbs ){
                                maxClassProbs = prob;
                                maxClassId    = static_cast<int>( k );
                            }
                        }

                        if( maxClassProbs > thres_i8 )
                        {
                            objProbs.push_back((deqnt_affine_to_f32(maxClassProbs, zp, scale)) * (deqnt_affine_to_f32(box_confidence, zp, scale)));
                            classId.push_back(maxClassId);
                            boxes.push_back(box_x);
                            boxes.push_back(box_y);
                            boxes.push_back(box_w);
                            boxes.push_back(box_h);
                            validCount++;
                        }
                    }
                }
            }
        }
        return validCount;
    }

    int YoloV5_Torch_Onnx_RKNN_NPU::PostProcessImplRKNN(
        int8_t* input_0, int8_t* input_1, int8_t* input_2, const int model_in_h, const int model_in_w,
        const std::vector<int32_t>& qnt_zps, const std::vector<float>& qnt_scales,
        std::vector<YOLOV5_RKNN::Detection>& results
    )
    {
        std::vector<float> filterBoxes;
        std::vector<float> objProbs;
        std::vector<int>   classId;

        // stride 8
        int stride0 = 8;
        int grid_h0 = model_in_h / stride0;
        int grid_w0 = model_in_w / stride0;
        const int validCount0
            = rknn_process_impl(
                input_0, anchor_0, grid_h0, grid_w0,
                model_in_h, model_in_w, stride0, filterBoxes, objProbs,
                classId, confidence_threshold_, qnt_zps[0], qnt_scales[0], class_num_
            );

        // stride 16
        int stride1 = 16;
        int grid_h1 = model_in_h / stride1;
        int grid_w1 = model_in_w / stride1;
        const int validCount1
            = rknn_process_impl(
                input_1, anchor_1, grid_h1, grid_w1,
                model_in_h, model_in_w, stride1, filterBoxes, objProbs,
                classId, confidence_threshold_, qnt_zps[1], qnt_scales[1], class_num_
            );

        // stride 32
        int validCount2 = 0;
        if( output_index_num_ == 3 && input_2 != nullptr ){
            int stride2 = 32;
            int grid_h2 = model_in_h / stride2;
            int grid_w2 = model_in_w / stride2;

            validCount2
                = rknn_process_impl(
                    input_2, anchor_2, grid_h2, grid_w2,
                    model_in_h, model_in_w, stride2, filterBoxes, objProbs,
                    classId, confidence_threshold_, qnt_zps[2], qnt_scales[2], class_num_
            );
        }

        const int validCount = validCount0 + validCount1 + validCount2;
        // no object detect
        if( validCount <= 0 ) {
            return 0;
        }

        std::vector<int> indexArray;
        for( int i = 0; i < validCount; ++i ) {
            indexArray.push_back(i);
        }
        sort_by_confidence_desc( objProbs, 0, validCount - 1, indexArray );

        std::set<int> class_set( std::begin(classId), std::end(classId) );
        for( auto c : class_set ) {
            nms( validCount, filterBoxes, classId, indexArray, c, nms_threshold_ );
        }

        /* box valid detect target */
        for( int i = 0; i < validCount; ++i ) {
            if( indexArray[i] == -1 ) continue;

            const int n = indexArray[i];

            // filterBoxes layout: [x_left_top, y_left_top, width, height] per detection.
            float x = filterBoxes[n * 4 + 0];
            float y = filterBoxes[n * 4 + 1];
            float w = filterBoxes[n * 4 + 2];
            float h = filterBoxes[n * 4 + 3];

            YOLOV5_RKNN::Detection result = {      // bbox, conf, class_id
                {x, y, w, h},
                objProbs[i],
                static_cast<float>(classId[n])
            };

            results.push_back( result );
        }
        return 0;
    }

    bool YoloV5_Torch_Onnx_RKNN_NPU::LoadModelEngineFile()
    {
        std::string engine_path = profile_.GetEngineFileName();
        if( engine_path.empty() == true ){
            MLOG_ERROR("Engine file path is empty in profile '%s'.", profile_.GetProfileName().c_str());
            return false;
        }
        if( IsValidFile( engine_path ) == false ){
            MLOG_ERROR("Engine file does not valid: '%s'", engine_path.c_str());
            return false;
        }

        std::ifstream engine_file( engine_path, std::ios::binary | std::ios::ate );
        if( !engine_file ){
            MLOG_ERROR("Failed to open engine file: %s", engine_path.c_str());
            return false;
        }

        std::streamsize file_size = engine_file.tellg();
        engine_file.seekg( 0, std::ios::beg );
        std::vector<char> engine_data( file_size );

        if( engine_file.read( engine_data.data(), file_size ).good() == false ){
            MLOG_ERROR("Failed to read engine file: %s (size: %lld)", engine_path.c_str(), file_size);
            return false;
        }
        engine_file.close();

		int ret = rknn_init( &rknn_ctx_, engine_data.data(), static_cast<size_t>( file_size ), 0, nullptr );
		if( ret < 0 ) {
            MLOG_ERROR("rknn_init() error ret=%d ( data size=%zu / file size=%lld )",
                    ret, engine_data.size(), static_cast<long long>(file_size));
			return false;
		}

		rknn_sdk_version version;
		ret = rknn_query( rknn_ctx_, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version) );
		if( ret < 0 ) {
			MLOG_ERROR("rknn_query RKNN_QUERY_SDK_VERSION error ret=%d", ret);
			return false;
		}
		MLOG_INFO("[SET] SDK version: %s, Driver version: %s", version.api_version, version.drv_version);

		// Multi-handler 환경 — handler 별 core 고정.
		//   handler N (device_id=N) 가 CORE_N 만 사용 → librknnrt 내부 mutex 회피.
		//   3 handler × 3 NPU core = 진정한 3-way parallel inference.
		//   참조: librknn_api/include/rknn_api.h:212-221.
		// 실패 시 치명 아님 — single core (AUTO) 로 정상 동작 가능, 경고만 남기고 계속.
		rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
		switch( device_id_ ) {
			case 0: core_mask = RKNN_NPU_CORE_0; break;
			case 1: core_mask = RKNN_NPU_CORE_1; break;
			case 2: core_mask = RKNN_NPU_CORE_2; break;
			default: core_mask = RKNN_NPU_CORE_AUTO; break;
		}
		ret = rknn_set_core_mask( rknn_ctx_, core_mask );
		if( ret < 0 ) {
			MLOG_WARN("rknn_set_core_mask(device=%d, mask=%d) failed ret=%d — fallback to AUTO", device_id_, (int)core_mask, ret);
		} else {
			MLOG_INFO("[SET] NPU device=%d core_mask=%d (fixed per-handler)", device_id_, (int)core_mask);
		}

		ret = rknn_query( rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &rknn_app_ctx_.io_num, sizeof(rknn_input_output_num) );
		if(ret < 0) {
			MLOG_ERROR("rknn_query RKNN_QUERY_IN_OUT_NUM error ret=%d", ret);
			return false;
		}
        if( batch_size_ != rknn_app_ctx_.io_num.n_input ){
			MLOG_ERROR("Target RKNN Engine batch_size[%d] != rknn_app_ctx_.io_num.n_input[%d]",
                batch_size_, rknn_app_ctx_.io_num.n_input );
			return false;
        }

        output_index_num_ = rknn_app_ctx_.io_num.n_output;

		MLOG_INFO("[SET] Model input num: %d, Output num: %d", rknn_app_ctx_.io_num.n_input, rknn_app_ctx_.io_num.n_output);

        return true;
    }

	static void dump_tensor_attr( rknn_tensor_attr* attr )
	{
        std::string shape_str = attr->n_dims < 1 ? "" : std::to_string( attr->dims[0] );

        for( unsigned int i = 1; i < attr->n_dims; ++i ){ shape_str += ", " + std::to_string( attr->dims[i] ); }

        MLOG_INFO(
                "  > index=%d, name=%-8s :: n_dims=%d, dims=[%s], n_elems=%d, "
                "size=%d, w_stride=%d, size_with_stride=%d, fmt=%s, type=%s, qnt_type=%s, zp=%d, scale=%f",

                // index name n_dims dims
                attr->index, attr->name, attr->n_dims, shape_str.c_str(),

                // n_elems size w_stride size_with_stride
                attr->n_elems, attr->size, attr->w_stride, attr->size_with_stride,

                // fmt type
                get_format_string(attr->fmt), get_type_string(attr->type),

                // qnt_type zp scale
                get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale );
	}

    bool YoloV5_Torch_Onnx_RKNN_NPU::AllocateBuffers()
    {
		int ret;

		rknn_tensor_attr* input_attrs = static_cast<rknn_tensor_attr*>( malloc( rknn_app_ctx_.io_num.n_input * sizeof(rknn_tensor_attr) ) );
		memset( input_attrs, 0, rknn_app_ctx_.io_num.n_input * sizeof(rknn_tensor_attr) );

		for( unsigned int i = 0; i < rknn_app_ctx_.io_num.n_input; i++ ) {
			input_attrs[i].index = i;
			ret = rknn_query( rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr) );
			if( ret < 0 ) {
			    MLOG_ERROR("rknn_query RKNN_QUERY_INPUT_ATTR error ret=%d", ret);
			    return false;
			}
			dump_tensor_attr( &(input_attrs[i]) );
		}

		rknn_tensor_attr* output_attrs = static_cast<rknn_tensor_attr*>( malloc( rknn_app_ctx_.io_num.n_output * sizeof(rknn_tensor_attr) ) );
		memset(output_attrs, 0, rknn_app_ctx_.io_num.n_output * sizeof(rknn_tensor_attr));

		for( unsigned int i = 0; i < rknn_app_ctx_.io_num.n_output; i++ ) {
			output_attrs[i].index = i;
			ret = rknn_query( rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr) );
			if( ret < 0 ) {
                MLOG_ERROR("rknn_query RKNN_QUERY_OUTPUT_ATTR error ret=%d", ret);
                return false;
			}
			dump_tensor_attr( &(output_attrs[i]) );
		}

        // parse class num
        class_num_ = ( output_attrs[0].dims[1] / 3 /*anchor each layer*/ ) - ( YOLOV5_RKNN::BBOX_ELEMENT_COUNT + YOLOV5_RKNN::CONF_ELEMENT_COUNT );
        MLOG_INFO("  > CLASS NUM : %d", class_num_);

		rknn_app_ctx_.input_attrs  = input_attrs;
		rknn_app_ctx_.output_attrs = output_attrs;

		if( input_attrs[0].fmt == RKNN_TENSOR_NCHW ) {
			MLOG_INFO("[SET] Model is NCHW input fmt");
			rknn_model_c = input_attrs[0].dims[1];
			rknn_model_h = input_attrs[0].dims[2];
			rknn_model_w = input_attrs[0].dims[3];
		}
		else {
			MLOG_INFO("[SET] Model is NHWC input fmt");
			rknn_model_h = input_attrs[0].dims[1];
			rknn_model_w = input_attrs[0].dims[2];
			rknn_model_c = input_attrs[0].dims[3];
		}
		MLOG_INFO("[SET] Model input height=%d, width=%d, channel=%d", rknn_model_h, rknn_model_w, rknn_model_c);

		if( rknn_outputs_ != nullptr ){
            delete[] rknn_outputs_;
			rknn_outputs_ = nullptr;
        }

        // set input memory
        for( size_t i = 0; i < rknn_app_ctx_.io_num.n_input; ++i )
        {
            rknn_input input;

            // set const value
            input.index        = i;
            input.type         = RKNN_TENSOR_UINT8;
            input.size         = rknn_model_w * rknn_model_h * rknn_model_c;
            input.fmt          = rknn_app_ctx_.input_attrs[i].fmt;
            input.pass_through = 0;

            // push back first
            rknn_inputs_.push_back( std::move( input ) );

            const size_t curr_idx = rknn_inputs_.size() - 1;

            // alloc memory
            rknn_input& target_input = rknn_inputs_[curr_idx];

            target_input.buf = malloc( input.size );

            if( target_input.buf == nullptr ){
                MLOG_ERROR("RKNN Input<%d> set error, memory alloc failed", i);
                return false;
            }
            memset( target_input.buf, 0, input.size );

            MLOG_INFO("RKNN Input memory<%d> set done", curr_idx);
        }

        // set output memory
		rknn_outputs_ = new rknn_output[rknn_app_ctx_.io_num.n_output];
		if( rknn_outputs_ == nullptr ) {
			MLOG_ERROR( "%s(), Malloc(rknn_outputs_) failed.", __FUNCTION__ );
			return false;
		}
        MLOG_INFO("RKNN Output memory set done");

        MLOG_INFO("NPU/Host buffers allocated successfully for engine '%s'.", profile_.GetProfileName().c_str());
        return true;
    }

    void YoloV5_Torch_Onnx_RKNN_NPU::ReleaseDeviceResources()
    {
		if( rknn_outputs_ != nullptr ) {
			delete[] rknn_outputs_;
			rknn_outputs_ = nullptr;
		}

		for( auto& input : rknn_inputs_ ){
            if( input.buf ){
                free( input.buf );
                input.buf = nullptr;
            }
        }
    }

    /**
     * @brief uchar* 타입의 BGR 이미지 데이터를 RGB로 In-place 변환합니다.
     * * @param data 이미지 데이터의 시작 포인터 (BGR 순서)
     * @param width 이미지의 너비
     * @param height 이미지의 높이
     */
    void bgrToRgbInPlace(unsigned char* data, int width, int height) {
        if (!data) {
            return; // 데이터가 없는 경우 종료
        }

        const int num_pixels = width * height;

        // 각 픽셀 (3바이트)을 순회하며 Blue와 Red 채널을 교체합니다.
        for (int i = 0; i < num_pixels; ++i) {
            // i번째 픽셀의 시작 주소
            unsigned char* pixel_ptr = data + i * 3;

            // pixel_ptr[0] (Blue)와 pixel_ptr[2] (Red)를 교환합니다.
            std::swap(pixel_ptr[0], pixel_ptr[2]);
        }
    }

    bool YoloV5_Torch_Onnx_RKNN_NPU::Preprocess( const InputLayerWrapper& input )
    {
        if( !input.image_data || input.image_data->empty() ){
            MLOG_WARN("Input image data is null or empty.");
            return false;
        }

        // 다음 입력을 둘 인덱스 (모듈러로 batch buffer 영역 안 보장)
        const size_t curr_batch_input_idx = current_batch_inputs_.size() % batch_size_;
        if( curr_batch_input_idx >= rknn_inputs_.size() ){
            MLOG_ERROR("Input memory buffer overflow! Idx=%zu, Size=%zu", curr_batch_input_idx, rknn_inputs_.size() );
            return false;
        }

        std::lock_guard<std::mutex> input_lck { rknn_input_lock_ };

        bgrToRgbInPlace( input.image_data->data(), static_cast<int>(rknn_model_w), static_cast<int>(rknn_model_h) );

        rknn_input& target_rknn_input = rknn_inputs_[curr_batch_input_idx];
        memset( target_rknn_input.buf, 0, target_rknn_input.size );
        memcpy( target_rknn_input.buf, input.image_data->data(), target_rknn_input.size );

        current_batch_inputs_.push_back( input );
        return current_batch_inputs_.size() >= batch_size_;
    }

    bool YoloV5_Torch_Onnx_RKNN_NPU::DoInference( )
    {
        {
            std::lock_guard<std::mutex> input_lck { rknn_input_lock_ };
            rknn_inputs_set( rknn_ctx_, rknn_app_ctx_.io_num.n_input, rknn_inputs_.data() );
        }

		memset(rknn_outputs_, 0, sizeof(rknn_output) * rknn_app_ctx_.io_num.n_output);
		for( auto i = 0; i < rknn_app_ctx_.io_num.n_output; i++ ) rknn_outputs_[i].want_float = 0;

		int ret = -99;

		ret = rknn_run(rknn_ctx_, nullptr);
		if( ret != RKNN_SUCC ){ MLOG_ERROR("rknn_run() return %d", ret); }

		ret = rknn_outputs_get(rknn_ctx_, rknn_app_ctx_.io_num.n_output, rknn_outputs_, nullptr);
		if( ret != RKNN_SUCC ){ MLOG_ERROR("rknn_outputs_get() return %d", ret); }

        return true;
    }

    std::vector<OutputLayerWrapper> YoloV5_Torch_Onnx_RKNN_NPU::Postprocess()
    {
        std::vector<OutputLayerWrapper> results;

        const size_t processed_batch_size = current_batch_inputs_.size();
        results.reserve( processed_batch_size ); // 결과 벡터 메모리 예약

        // 처리할 입력이 없거나 Host 버퍼가 준비되지 않은 경우
        if( processed_batch_size == 0 ){
            MLOG_WARN("Postprocess called with empty batch.");
            return results;
        }

        std::vector<int32_t> out_zps;
        std::vector<float>   out_scales;
        std::vector<YOLOV5_RKNN::Detection> raw_results;

        for( unsigned int i = 0; i < rknn_app_ctx_.io_num.n_output; ++i ) {
            out_scales.push_back( rknn_app_ctx_.output_attrs[i].scale );
            out_zps.push_back( rknn_app_ctx_.output_attrs[i].zp );
        }

        // batch 맞음?? PostProcessImplRKNN 내부에서도 out_zps 같은 거 0 1 2 안해야할거같은데
        if( output_index_num_ == 3 ){
            PostProcessImplRKNN(
                static_cast<int8_t*>( rknn_outputs_[0].buf ),
                static_cast<int8_t*>( rknn_outputs_[1].buf ),
                static_cast<int8_t*>( rknn_outputs_[2].buf ),
                rknn_model_h, rknn_model_w, out_zps, out_scales, raw_results );
        }
        else if( output_index_num_ == 2 ){
            PostProcessImplRKNN(
                static_cast<int8_t*>( rknn_outputs_[0].buf ),
                static_cast<int8_t*>( rknn_outputs_[1].buf ),
                nullptr,
                rknn_model_h, rknn_model_w, out_zps, out_scales, raw_results );
        }
        else {
            MLOG_ERROR("output_index_num_ = %d, PostProcessImplRKNN not in case", output_index_num_);
        }

        int ret = rknn_outputs_release( rknn_ctx_, rknn_app_ctx_.io_num.n_output, rknn_outputs_ );
        if( ret < 0 ){
            MLOG_ERROR("rknn_outputs_release failed");
        }

        for( size_t i = 0; i < processed_batch_size; ++i )
        {
            const auto& input_wrapper = current_batch_inputs_[i]; // 원본 입력 정보
            std::vector<InferObject> detected_objects;            // 현재 이미지의 최종 탐지 결과

            for( const auto& raw_result : raw_results )
            {
                InferObject infer_object;
                infer_object.bbox.x          = raw_result.bbox[0];
                infer_object.bbox.y          = raw_result.bbox[1];
                infer_object.bbox.w          = raw_result.bbox[2];
                infer_object.bbox.h          = raw_result.bbox[3];
                infer_object.xy_ref_type     = BBoxReferenceType::ltx_type;
                infer_object.coord_format    = BBoxCoordinateFormat::pixel_int;
                infer_object.score           = raw_result.conf;
                infer_object.class_id        = raw_result.class_id;
                infer_object.infer_engine_id = profile_.GetProfileUUID();
                infer_object.track_id        = INFER_TRACK_ID_NOT_SET;

                detected_objects.push_back( infer_object );
            }

            // OutputLayerWrapper 생성 (탐지된 객체가 없어도 생성)
            results.push_back(
                OutputLayerWrapper::Build
                (
                    input_wrapper.meta_data, // 원본 요청 메타데이터
                    GetEngineHandleUUID(),   // 현재 핸들러 UUID
                    std::move(detected_objects)  // 탐지 결과 벡터 이동
                )
            );
        }
        // current_batch_inputs_ 는 InferenceThreadRunner 루프 마지막에 clear됨
        return results;
    }

// YoloV5_Torch_Onnx_RKNN_NPU
} // namespace MGEN
