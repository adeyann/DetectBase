#include "InferObject.h"
#include "MgenLogger.h"

namespace MGEN
{
    InferObject::InferObject( const InferObject& other )
        : infer_engine_id ( other.infer_engine_id )
        , class_id        ( other.class_id )
        , track_id        ( other.track_id )
        , score           ( other.score )
        , bbox            ( other.bbox )          // Calls InferBBox copy constructor (defaulted)
        , xy_ref_type     ( other.xy_ref_type )
        , coord_format    ( other.coord_format )
        , extend_data     ( other.extend_data )   // Calls std::vector copy constructor (deep copy)
    {
        //
    }

    InferObject& InferObject::operator=( const InferObject& other )
    {
        if( this != &other ){ // Protect against self-assignment
            infer_engine_id = other.infer_engine_id;
            class_id        = other.class_id;
            track_id        = other.track_id;
            score           = other.score;
            bbox            = other.bbox;        // Calls InferBBox copy assignment (defaulted)
            xy_ref_type     = other.xy_ref_type;
            coord_format    = other.coord_format;
            extend_data     = other.extend_data; // Calls std::vector copy assignment (deep copy)
        }
        return *this;
    }

    InferObject::InferObject( InferObject&& other ) noexcept
        : infer_engine_id ( other.infer_engine_id ) // Copy plain types
        , class_id        ( other.class_id )
        , track_id        ( other.track_id )
        , score           ( other.score )
        , bbox            ( other.bbox ) // performance-move-const-arg: InferBBox 는 trivially-copyable → move 무효
        , xy_ref_type     ( other.xy_ref_type )
        , coord_format    ( other.coord_format )
        , extend_data     ( std::move(other.extend_data) ) // Call std::vector move constructor (resource transfer, noexcept)
    {
        // Reset the source object to a defined state after move
        other.infer_engine_id = INFER_ENGINE_ID_NOT_SET;
        other.class_id        = INFER_CLASS_ID_NOT_SET;
        other.track_id        = INFER_TRACK_ID_NOT_SET;
        other.score           = 0.0f;
        // bbox is moved from (default state)
        other.xy_ref_type     = BBoxReferenceType::ltx_type; // Reset enum
        other.coord_format    = BBoxCoordinateFormat::pixel_int; // Reset enum
        // extend_data is moved from (empty state)
    }

    InferObject& InferObject::operator=( InferObject&& other ) noexcept
    {
        if( this != &other ){ // Protect against self-assignment
            // Release current resources (implicitly done by vector/bbox move assignment)

            // Move resources from other
            infer_engine_id = other.infer_engine_id; // Copy plain types
            class_id        = other.class_id;
            track_id        = other.track_id;
            score           = other.score;
            bbox            = other.bbox; // performance-move-const-arg: InferBBox 는 trivially-copyable → move 무효
            xy_ref_type     = other.xy_ref_type;
            coord_format    = other.coord_format;
            extend_data     = std::move(other.extend_data); // Call std::vector move assignment (resource transfer, noexcept)

            // Reset the source object to a defined state after move
            other.infer_engine_id = INFER_ENGINE_ID_NOT_SET;
            other.class_id        = INFER_CLASS_ID_NOT_SET;
            other.track_id        = INFER_TRACK_ID_NOT_SET;
            other.score           = 0.0f;
            other.xy_ref_type     = BBoxReferenceType::ltx_type;
            other.coord_format    = BBoxCoordinateFormat::pixel_int;
        }
        return *this;
    }

    bool operator==( const InferObject& lhs, const InferObject& rhs )
    {
        return lhs.infer_engine_id == rhs.infer_engine_id
            && lhs.class_id == rhs.class_id
            && lhs.track_id == rhs.track_id
            && are_equal_float( lhs.score, rhs.score )
            && are_equal_float( lhs.bbox.x, rhs.bbox.x )
            && are_equal_float( lhs.bbox.y, rhs.bbox.y )
            && are_equal_float( lhs.bbox.w, rhs.bbox.w )
            && are_equal_float( lhs.bbox.h, rhs.bbox.h )
            && lhs.xy_ref_type == rhs.xy_ref_type
            && lhs.coord_format == rhs.coord_format;
    }

    // --- Coordinate Conversion Helper Functions ---

    // 내부 헬퍼 함수: 입력 BBox를 중간 형식(ratio_float, ltx_type)으로 변환
    static bool ConvertToIntermediateRatioLtx(
        const InferBBox& bbox, BBoxReferenceType ref_type, BBoxCoordinateFormat format,
        int src_width, int src_height, float& ratio_ltx_x, float& ratio_ltx_y, float& ratio_w, float& ratio_h )
    {
        if( src_width <= 0 || src_height <= 0 ){
            MLOG_ERROR("ConvertToIntermediateRatioLtx: Source dimensions must be positive (%dx%d).", src_width, src_height);
            return false;
        }
        float src_w_f = static_cast<float>(src_width);
        float src_h_f = static_cast<float>(src_height);

        float temp_ratio_x, temp_ratio_y, temp_ratio_w, temp_ratio_h;

        // 픽셀 -> 비율
        if( format == BBoxCoordinateFormat::pixel_int )
        {
            temp_ratio_x = bbox.x / src_w_f;
            temp_ratio_y = bbox.y / src_h_f;
            temp_ratio_w = bbox.w / src_w_f;
            temp_ratio_h = bbox.h / src_h_f;
        }
        else if( format == BBoxCoordinateFormat::ratio_float )
        {
            temp_ratio_x = bbox.x;
            temp_ratio_y = bbox.y;
            temp_ratio_w = bbox.w;
            temp_ratio_h = bbox.h;
        }
        else
        {
            MLOG_ERROR("ConvertToIntermediateRatioLtx: Invalid source format (%d).", static_cast<int>(format));
            return false;
        }

        // 센터 -> 좌상단
        if( ref_type == BBoxReferenceType::cx_type )
        {
            ratio_ltx_x = temp_ratio_x - temp_ratio_w / 2.0f;
            ratio_ltx_y = temp_ratio_y - temp_ratio_h / 2.0f;
        }
        else if( ref_type == BBoxReferenceType::ltx_type )
        {
            ratio_ltx_x = temp_ratio_x;
            ratio_ltx_y = temp_ratio_y;
        }
        else
        {
            MLOG_ERROR("ConvertToIntermediateRatioLtx: Invalid source reference type (%d).", static_cast<int>(ref_type));
            return false;
        }

        ratio_w = temp_ratio_w;
        ratio_h = temp_ratio_h;

        return true;
    }

    // 내부 헬퍼 함수: 중간 형식(ratio_float, ltx_type) -> 최종 목표 형식으로 변환
    static bool ConvertFromIntermediateRatioLtx(
        float ratio_ltx_x, float ratio_ltx_y, float ratio_w, float ratio_h,
        const ImageExpressStyle& dst_style, // 목표 스타일 정보 사용
        InferBBox& result_bbox )            // 결과를 직접 수정
    {
        if( dst_style.width <= 0 || dst_style.height <= 0 )
        {
            MLOG_ERROR("ConvertFromIntermediateRatioLtx: Destination dimensions must be positive (%dx%d).", dst_style.width, dst_style.height);
            return false;
        }
        float dst_w_f = static_cast<float>( dst_style.width  );
        float dst_h_f = static_cast<float>( dst_style.height );

        float temp_ratio_x, temp_ratio_y, temp_ratio_w, temp_ratio_h;

        temp_ratio_w = ratio_w;
        temp_ratio_h = ratio_h;

        // 좌상단 -> 센터 (dst_style.ref_type 사용)
        if( dst_style.ref_type == BBoxReferenceType::cx_type )
        {
            temp_ratio_x = ratio_ltx_x + ratio_w / 2.0f;
            temp_ratio_y = ratio_ltx_y + ratio_h / 2.0f;
        }
        else if( dst_style.ref_type == BBoxReferenceType::ltx_type )
        {
            temp_ratio_x = ratio_ltx_x;
            temp_ratio_y = ratio_ltx_y;
        }
        else
        {
            MLOG_ERROR("ConvertFromIntermediateRatioLtx: Invalid target reference type (%d).", static_cast<int>(dst_style.ref_type));
            return false;
        }

        // 비율 -> 픽셀 (dst_style.format 사용)
        if( dst_style.format == BBoxCoordinateFormat::pixel_int )
        {
            temp_ratio_x *= dst_w_f;
            temp_ratio_y *= dst_h_f;
            temp_ratio_w *= dst_w_f;
            temp_ratio_h *= dst_h_f;
        }
        else if( dst_style.format != BBoxCoordinateFormat::ratio_float )
        {
            MLOG_ERROR("ConvertFromIntermediateRatioLtx: Invalid target coordinate format (%d).", static_cast<int>(dst_style.format));
            return false;
        }

        // 결과 BBox 업데이트
        result_bbox.x = temp_ratio_x;
        result_bbox.y = temp_ratio_y;
        result_bbox.w = temp_ratio_w;
        result_bbox.h = temp_ratio_h;

        // 형식과 기준점은 호출부에서 dst_style 기준으로 업데이트
        return true;
    }

    // --- Main Conversion Functions ---

    bool ConvertInferObjectCoordinate( MGEN::InferObject& object, const ImageExpressStyle& src_style, const ImageExpressStyle& dst_style )
    {
            // 현재 객체의 bbox, 형식, 기준점 가져오기
            const InferBBox&           current_bbox     = object.bbox; // Use const ref
            const BBoxReferenceType    current_ref_type = object.xy_ref_type;
            const BBoxCoordinateFormat current_format   = object.coord_format;

            // --- 변환 필요 여부 확인 ---
            // 1. 기본 속성 비교 (format, ref_type, dimensions, padded_flag)
            const bool basic_properties_match =
                (
                    current_format                == dst_style.format   &&
                    current_ref_type              == dst_style.ref_type &&
                    src_style.width               == dst_style.width    &&
                    src_style.height              == dst_style.height   &&
                    src_style.is_padded_or_scaled == dst_style.is_padded_or_scaled
                );

            // 2. 패딩/스케일링 세부 정보 비교 (basic_properties_match가 true이고, 패딩이 사용된 경우에만 필요)
            bool padding_details_match = false;
            if( basic_properties_match && src_style.is_padded_or_scaled )
            {
                // Assumes are_equal_float is available from math_utils.h
                padding_details_match =
                    are_equal_float( src_style.scale_factor_from_original, dst_style.scale_factor_from_original ) &&
                    src_style.pad_x_offset    == dst_style.pad_x_offset   &&
                    src_style.pad_y_offset    == dst_style.pad_y_offset   &&
                    src_style.original_width  == dst_style.original_width &&
                    src_style.original_height == dst_style.original_height;
            }

            // 3. 최종 변환 필요 여부 결정
            // 변환 불필요 조건: 기본 속성이 일치하고, (패딩이 없거나 || 패딩 세부 정보도 일치하는 경우)
            const bool styles_are_identical = basic_properties_match && ( !src_style.is_padded_or_scaled || padding_details_match );

            if( styles_are_identical ){
                return true; // No conversion needed, considered successful
            }

            // --- 1단계: 입력을 중간 형식 (ratio_float, ltx_type)으로 변환 ---
            float ratio_ltx_x, ratio_ltx_y, ratio_w, ratio_h;
            if( ConvertToIntermediateRatioLtx( current_bbox, current_ref_type, current_format,
                    src_style.width, src_style.height, ratio_ltx_x, ratio_ltx_y, ratio_w, ratio_h ) == false ){
                MLOG_WARN("Failed to convert object to intermediate format (id:%d, class:%d). Skipping.", object.track_id, object.class_id);
                return false; // Indicate failure
            }

            // --- 2단계: 패딩/스케일링 역변환 또는 정변환 적용 (필요 시) ---
            // This logic adjusts the intermediate ratio coordinates based on padding/scaling differences.
            // The resulting ratios are relative to the *destination* style's coordinate system
            // (either original or padded, depending on dst_style.is_padded_or_scaled).

            // Case 1: 소스=패딩, 목적=원본 (Padded -> Original)
            if( src_style.is_padded_or_scaled && !dst_style.is_padded_or_scaled ){
                 // Check for valid parameters needed for this conversion
                if( src_style.scale_factor_from_original <= 1e-6f ||
                    dst_style.width <= 0 || dst_style.height <= 0 ||
                    src_style.width <= 0 || src_style.height <= 0 ||
                    src_style.original_width  <= 0 ||
                    src_style.original_height <= 0 ||
                    dst_style.width  != src_style.original_width ||
                    dst_style.height != src_style.original_height )
                {   // Destination must match original dims
                    MLOG_WARN("Invalid parameters for padded to original conversion (Src: %dx%d, Pad:%d,%d, Scale:%.2f, Orig:%dx%d; Dst: %dx%d). Skipping padding/scale adjustment for object (id:%d, class:%d).",
                        src_style.width, src_style.height, src_style.pad_x_offset, src_style.pad_y_offset, src_style.scale_factor_from_original,
                        src_style.original_width, src_style.original_height, dst_style.width, dst_style.height, object.track_id, object.class_id);
                    // Proceed without padding adjustment, might lead to incorrect coords if dimensions mismatch
                }
                else
                {
                    // Convert intermediate ratios (relative to padded src) to pixels in padded src
                    float px1_padded = ratio_ltx_x * static_cast<float>( src_style.width );
                    float py1_padded = ratio_ltx_y * static_cast<float>( src_style.height );
                    float pw_padded  = ratio_w * static_cast<float>( src_style.width );
                    float ph_padded  = ratio_h * static_cast<float>( src_style.height );

                    // Remove padding offset
                    float px1_scaled = px1_padded - static_cast<float>( src_style.pad_x_offset );
                    float py1_scaled = py1_padded - static_cast<float>( src_style.pad_y_offset );
                    // (width/height remain scaled, offset doesn't change size)

                    // Reverse scaling to get coordinates in original image space
                    // Avoid division by zero for scale factor
                    float inv_scale = 1.0f / src_style.scale_factor_from_original;
                    float x1_orig_f = px1_scaled * inv_scale;
                    float y1_orig_f = py1_scaled * inv_scale;
                    float w_orig_f  = pw_padded * inv_scale; // Width/height are scaled directly
                    float h_orig_f  = ph_padded * inv_scale;

                    // Recalculate ratios relative to the destination (original) dimensions
                    // Avoid division by zero for destination dimensions
                    float inv_dst_w = 1.0f / static_cast<float>( dst_style.width );
                    float inv_dst_h = 1.0f / static_cast<float>( dst_style.height );

                    ratio_ltx_x = x1_orig_f * inv_dst_w; // dst_style.width == src_style.original_width
                    ratio_ltx_y = y1_orig_f * inv_dst_h; // dst_style.height == src_style.original_height

                    ratio_w = w_orig_f * inv_dst_w;
                    ratio_h = h_orig_f * inv_dst_h;
                }
            }
            // Case 2: 소스=원본, 목적=패딩 (Original -> Padded)
            else if( !src_style.is_padded_or_scaled && dst_style.is_padded_or_scaled )
            {
                // Check for valid parameters needed for this conversion
                if( dst_style.scale_factor_from_original <= 1e-6f ||
                    src_style.width <= 0 || src_style.height <= 0 ||
                    dst_style.width <= 0 || dst_style.height <= 0 ||
                    dst_style.original_width  <= 0 ||
                    dst_style.original_height <= 0 ||
                    src_style.width  != dst_style.original_width ||
                    src_style.height != dst_style.original_height )
                {   // Source must match original dims for dest
                    MLOG_WARN("Invalid parameters for original to padded conversion (Src: %dx%d; Dst: %dx%d, Pad:%d,%d, Scale:%.2f, Orig:%dx%d). Skipping padding/scale adjustment for object (id:%d, class:%d).",
                        src_style.width, src_style.height, dst_style.width, dst_style.height, dst_style.pad_x_offset, dst_style.pad_y_offset,
                        dst_style.scale_factor_from_original, dst_style.original_width, dst_style.original_height, object.track_id, object.class_id);
                     // Proceed without padding adjustment, might lead to incorrect coords if dimensions mismatch
                }
                else
                {
                    // Convert intermediate ratios (relative to original src) to pixels in original src
                    float x1_orig_f = ratio_ltx_x * static_cast<float>( src_style.width ); // src_style.width == dst_style.original_width
                    float y1_orig_f = ratio_ltx_y * static_cast<float>( src_style.height ); // src_style.height == dst_style.original_height
                    float w_orig_f  = ratio_w * static_cast<float>( src_style.width );
                    float h_orig_f  = ratio_h * static_cast<float>( src_style.height );

                    // Apply scaling
                    float px1_scaled = x1_orig_f * dst_style.scale_factor_from_original;
                    float py1_scaled = y1_orig_f * dst_style.scale_factor_from_original;
                    float pw_scaled  = w_orig_f * dst_style.scale_factor_from_original; // Width/height are scaled directly
                    float ph_scaled  = h_orig_f * dst_style.scale_factor_from_original;

                    // Add padding offset
                    float px1_padded = px1_scaled + static_cast<float>( dst_style.pad_x_offset );
                    float py1_padded = py1_scaled + static_cast<float>( dst_style.pad_y_offset );
                    // (width/height remain scaled, offset doesn't change size)

                    // Recalculate ratios relative to the destination (padded) dimensions
                    // Avoid division by zero for destination dimensions
                    float inv_dst_w = 1.0f / static_cast<float>( dst_style.width );
                    float inv_dst_h = 1.0f / static_cast<float>( dst_style.height );

                    ratio_ltx_x = px1_padded * inv_dst_w;
                    ratio_ltx_y = py1_padded * inv_dst_h;

                    ratio_w = pw_scaled * inv_dst_w;
                    ratio_h = ph_scaled * inv_dst_h;
                }
            }
            // Case 3: 소스=패딩, 목적=패딩 (Padded -> Padded) or 소스=원본, 목적=원본 (Original -> Original)
            // If dimensions differ OR format/ref_type differ, padding/scale logic is skipped,
            // but the format/ref_type/dimension conversion still happens in step 3 using the intermediate ratios.
            // If src/dst have *different* padding/scaling parameters, this case doesn't explicitly handle
            // converting between two *different* padded coordinate systems. It assumes either
            // Padded -> Original or Original -> Padded, or that if both are padded/original,
            // the padding/scaling parameters are implicitly the same if is_padded_or_scaled flags match AND dimensions match.
            // A more complex Padded -> DifferentPadded conversion would require combining steps (Padded -> Original -> DifferentPadded).

            // --- 3단계: 중간 형식 -> 최종 목표 형식으로 변환 (dst_style 사용) ---
            InferBBox final_bbox;
            if( ConvertFromIntermediateRatioLtx( ratio_ltx_x, ratio_ltx_y, ratio_w, ratio_h, dst_style, final_bbox ) == false )
            {
                MLOG_WARN("Failed to convert object from intermediate format (id:%d, class:%d). Skipping.", object.track_id, object.class_id);
                return false; // Indicate failure
            }

            // --- 4단계: 변환된 값으로 InferObject 업데이트 ---
            object.bbox         = final_bbox;         // 계산된 최종 bbox 값으로 업데이트
            object.coord_format = dst_style.format;   // 목표 형식으로 업데이트
            object.xy_ref_type  = dst_style.ref_type; // 목표 기준점으로 업데이트

            // --- 4a. (선택적) 최종 좌표 클램핑 ---
            // Clamp coordinates to be within the bounds of the destination image/coordinate space.
            if( object.coord_format == BBoxCoordinateFormat::pixel_int && dst_style.width > 0 && dst_style.height > 0 )
            {
                // Clamp pixel coordinates to [0, width] and [0, height] range.
                // Note: Clamping to dim-1 vs dim depends on whether the coordinate represents the start or end of a pixel.
                // Common practice is [0, dim), so clamping max to dim is often used. Let's use [0, dim].
                float max_x = static_cast<float>( dst_style.width  );
                float max_y = static_cast<float>( dst_style.height );

                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;

                // Convert current bbox representation (could be ltx or cx) to x1, y1, x2, y2
                if( object.xy_ref_type == BBoxReferenceType::ltx_type )
                {
                    x1 = object.bbox.x;
                    y1 = object.bbox.y;
                    x2 = x1 + object.bbox.w;
                    y2 = y1 + object.bbox.h;
                }
                else
                {   // cx_type
                    x1 = object.bbox.x - object.bbox.w / 2.0f;
                    y1 = object.bbox.y - object.bbox.h / 2.0f;
                    x2 = object.bbox.x + object.bbox.w / 2.0f;
                    y2 = object.bbox.y + object.bbox.h / 2.0f;
                }

                // Clamp the corners
                float clamped_x1 = std::max( 0.0f, std::min( x1, max_x ) );
                float clamped_y1 = std::max( 0.0f, std::min( y1, max_y ) );
                float clamped_x2 = std::max( clamped_x1, std::min( x2, max_x ) ); // Ensure x2 >= x1 after clamping
                float clamped_y2 = std::max( clamped_y1, std::min( y2, max_y ) ); // Ensure y2 >= y1 after clamping

                // Update bbox based on clamped values and the *destination* ref_type
                float clamped_w = clamped_x2 - clamped_x1;
                float clamped_h = clamped_y2 - clamped_y1;

                if( object.xy_ref_type == BBoxReferenceType::ltx_type )
                {   // Which is now dst_style.ref_type
                    object.bbox.x = clamped_x1;
                    object.bbox.y = clamped_y1;
                    object.bbox.w = clamped_w;
                    object.bbox.h = clamped_h;
                }
                else
                {   // cx_type // Which is now dst_style.ref_type
                    object.bbox.x = clamped_x1 + clamped_w / 2.0f; // Recalculate center x
                    object.bbox.y = clamped_y1 + clamped_h / 2.0f; // Recalculate center y
                    object.bbox.w = clamped_w;
                    object.bbox.h = clamped_h;
                }

            }
            else if( object.coord_format == BBoxCoordinateFormat::ratio_float )
            {
                // Clamp ratio coordinates to [0.0, 1.0] range.
                float max_coord = 1.0f;
                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;

                if( object.xy_ref_type == BBoxReferenceType::ltx_type )
                {
                    x1 = object.bbox.x;
                    y1 = object.bbox.y;
                    x2 = x1 + object.bbox.w;
                    y2 = y1 + object.bbox.h;
                }
                else
                {   // cx_type
                    x1 = object.bbox.x - object.bbox.w / 2.0f;
                    y1 = object.bbox.y - object.bbox.h / 2.0f;
                    x2 = object.bbox.x + object.bbox.w / 2.0f;
                    y2 = object.bbox.y + object.bbox.h / 2.0f;
                }

                // Clamp the corners
                float clamped_x1 = std::max( 0.0f, std::min( x1, max_coord ) );
                float clamped_y1 = std::max( 0.0f, std::min( y1, max_coord ) );
                float clamped_x2 = std::max( clamped_x1, std::min( x2, max_coord ) ); // Ensure x2 >= x1
                float clamped_y2 = std::max( clamped_y1, std::min( y2, max_coord ) ); // Ensure y2 >= y1

                // Update bbox based on clamped values and the *destination* ref_type
                float clamped_w = clamped_x2 - clamped_x1;
                float clamped_h = clamped_y2 - clamped_y1;

                if( object.xy_ref_type == BBoxReferenceType::ltx_type )
                {   // Which is now dst_style.ref_type
                    object.bbox.x = clamped_x1;
                    object.bbox.y = clamped_y1;
                    object.bbox.w = clamped_w;
                    object.bbox.h = clamped_h;
                }
                else
                {   // cx_type // Which is now dst_style.ref_type
                    object.bbox.x = clamped_x1 + clamped_w / 2.0f;
                    object.bbox.y = clamped_y1 + clamped_h / 2.0f;
                    object.bbox.w = clamped_w;
                    object.bbox.h = clamped_h;
                }
            }

            return true; // Conversion successful
    }

    void ConvertInferObjectsCoordinates( std::vector<MGEN::InferObject>& objects, const ImageExpressStyle& src_style, const ImageExpressStyle& dst_style )
    {
        // Basic check if styles might be identical (can skip iteration if they are)
        // Note: This is a preliminary check. ConvertInferObjectCoordinate does the definitive check.
        bool potentially_identical =
            (
                src_style.format              == dst_style.format   &&
                src_style.ref_type            == dst_style.ref_type &&
                src_style.width               == dst_style.width    &&
                src_style.height              == dst_style.height   &&
                src_style.is_padded_or_scaled == dst_style.is_padded_or_scaled
            );

        if( potentially_identical && src_style.is_padded_or_scaled )
        {
            potentially_identical =
                are_equal_float(src_style.scale_factor_from_original, dst_style.scale_factor_from_original) &&
                src_style.pad_x_offset    == dst_style.pad_x_offset   &&
                src_style.pad_y_offset    == dst_style.pad_y_offset   &&
                src_style.original_width  == dst_style.original_width &&
                src_style.original_height == dst_style.original_height;
        }

        if( potentially_identical ){
             // It's highly likely no conversion is needed for any object assuming they all start
             // with the format/type defined by src_style. However, individual objects *could*
             // theoretically have different initial formats/types despite the src_style hint.
             // For absolute safety, iterating is best unless performance is critical.
             // If skipping iteration: return;
        }

        int failure_count = 0;
        for( auto& obj : objects ){
            // Call the single object conversion function.
            if( ConvertInferObjectCoordinate( obj, src_style, dst_style ) == false ){
                failure_count++;
            }
        }

        if (failure_count > 0) {
            MLOG_WARN("Coordinate conversion failed for %d out of %zu objects.", failure_count, objects.size());
        }
    }

} // namespace MGEN
