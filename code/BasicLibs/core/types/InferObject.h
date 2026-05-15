#ifndef _MGEN_INFER_OBJECT_STRUCTURE_H_
#define _MGEN_INFER_OBJECT_STRUCTURE_H_

#include "math_utils.h"
#include "MgenTypes.h"

#include <vector>
#include <utility>    // For std::move
#include <functional> // std::hash

namespace MGEN
{
    // --- Provided Definitions from Header ---

    enum class BBoxReferenceType : unsigned char
    {
        cx_type,  // center based x, y : cx, cy
        ltx_type  // letf-top based x, y : ltx, lty
    };

    enum class BBoxCoordinateFormat : unsigned char
    {
        ratio_float, // ratio : 0.0f ~ 1.0f
        pixel_int    // image size based pixel coord
    };

    /**
     * @brief Structure for bounding box coordinates.
     * @details Contains float members, so default copy/move/destructor are sufficient (Rule of Zero).
     */
    struct InferBBox
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

    using InferClassID  = int;
    using InferTrackID  = int;
    using InferScore    = float;
    using InferEngineID = unsigned int;

    constexpr InferClassID  INFER_CLASS_ID_NOT_SET = -1;
    constexpr InferTrackID  INFER_TRACK_ID_NOT_SET = -2;
    constexpr InferEngineID INFER_ENGINE_ID_NOT_SET = 0;
    constexpr InferEngineID INFER_ENGINE_ID_INITIAL = 1;

    /**
     * @brief Structure to hold inference results for a detected object within the MGEN namespace.
     *
     * Contains metadata like engine ID, class ID, tracking ID, confidence score,
     * bounding box information, and optional extended data. Includes Rule of Five implementation.
     */
    struct InferObject // 56 byte
    {
        // --- Member Variables -----------------------------------------------------------------------
        // inference results metadata
        InferEngineID        infer_engine_id = INFER_ENGINE_ID_NOT_SET;
        InferClassID         class_id        = INFER_CLASS_ID_NOT_SET;
        InferTrackID         track_id        = INFER_TRACK_ID_NOT_SET;
        InferScore           score           = 0.0f;
        // bbox
        InferBBox            bbox;
        BBoxReferenceType    xy_ref_type     = BBoxReferenceType::ltx_type;
        BBoxCoordinateFormat coord_format    = BBoxCoordinateFormat::pixel_int;
        // Extended data
        std::vector<float>   extend_data; // if empty, 24byte

        // --- Rule of Five Implementation ------------------------------------------------------------
        InferObject()  = default;
        ~InferObject() = default;

        /**
         * @brief Copy constructor.
         * @details Performs a deep copy of all members.
         * @param other The InferObject to copy from.
         */
        InferObject( const InferObject& other );

        /**
         * @brief Copy assignment operator.
         * @details Performs a deep copy assignment. Handles self-assignment.
         * @param other The InferObject to copy assign from.
         * @return Reference to *this.
         */
        InferObject& operator=( const InferObject& other );

        /**
         * @brief Move constructor.
         * @details Transfers ownership of resources (bbox, extend_data) from 'other'.
         * 'other' is left in a valid, default state. Marked noexcept for performance.
         * @param other The rvalue reference to the InferObject to move from.
         */
        InferObject( InferObject&& other ) noexcept;

        /**
         * @brief Move assignment operator.
         * @details Transfers ownership of resources from 'other' to this object.
         * Releases resources previously held by this object. 'other' is left in a valid, default state.
         * Marked noexcept for performance.
         * @param other The rvalue reference to the InferObject to move assign from.
         * @return Reference to *this.
         */
        InferObject& operator=( InferObject&& other ) noexcept;
    };

    bool operator==( const InferObject& lhs, const InferObject& rhs );

    using ImageExpressCoordFormat   = BBoxCoordinateFormat;
    using ImageExpressReferenceType = BBoxReferenceType;

    /**
     * @brief 이미지 또는 좌표계의 표현 스타일 및 관련 정보를 정의하는 구조체.
     * 좌표 변환 함수의 소스(Source) 및 목적(Destination) 스타일을 지정하는 데 사용됩니다.
     */
    struct ImageExpressStyle
    {
        int width  = 0; //< 이 좌표계의 기준 너비 (픽셀 변환 시 사용)
        int height = 0; //< 이 좌표계의 기준 높이 (픽셀 변환 시 사용)
        ImageExpressCoordFormat   format   = BBoxCoordinateFormat::pixel_int; //< 좌표 형식   (ratio_float 또는 pixel_int)
        ImageExpressReferenceType ref_type = BBoxReferenceType::ltx_type;     //< 기준점 형식 (ltx_type 또는 cx_type)

        // 참고: 실제 좌표 형식(ratio/pixel)과 기준점(ltx/cx)은 변환 대상인
        //       InferObject 자체에 저장된 값을 사용합니다 (소스 기준).
        //       변환 목표 형식/기준점은 변환 함수 호출 시 파라미터로 지정합니다.

        // --- 아래는 이 좌표계가 다른 "원본" 좌표계로부터 패딩/스케일링된 경우에만 관련됨 ---
        bool  is_padded_or_scaled        = false; //< 원본에서 변환된 좌표계인지 여부
        float scale_factor_from_original = 1.0f;  //< 원본에서 현재 크기로 적용된 스케일 비율
        int   pad_x_offset               = 0;     //< 현재 좌표계 기준 X축 패딩 오프셋 (픽셀)
        int   pad_y_offset               = 0;     //< 현재 좌표계 기준 Y축 패딩 오프셋 (픽셀)
        int   original_width             = 0;     //< 대응되는 원본 이미지 너비
        int   original_height            = 0;     //< 대응되는 원본 이미지 높이

        // 기본 생성자
        ImageExpressStyle() = default;

        // 생성자 (간편 초기화용)
        ImageExpressStyle(
            int w,
            int h,
            ImageExpressCoordFormat   fmt = BBoxCoordinateFormat::pixel_int,
            ImageExpressReferenceType ref = BBoxReferenceType::ltx_type,
            bool  padded = false,
            float scale  = 1.0f,
            int px = 0,
            int py = 0,
            int ow = 0,
            int oh = 0)
        : width(w), height(h), format(fmt), ref_type(ref), is_padded_or_scaled(padded), scale_factor_from_original(scale)
        , pad_x_offset(px), pad_y_offset(py), original_width(ow), original_height(oh)
        {
            // 원본 크기가 주어지지 않으면 현재 크기로 가정 (패딩/스케일된 경우)
            if( is_padded_or_scaled && original_width  == 0 && width  > 0 ) original_width  = width;
            if( is_padded_or_scaled && original_height == 0 && height > 0 ) original_height = height;
        }
    };

    /**
     * @brief 단일 InferObject의 바운딩 박스 좌표를 소스 스타일에서 목적 스타일로 변환합니다.
     * 객체의 형식/기준점 정보를 읽고, 목적 스타일에 정의된 형식/기준점으로 변환 후 객체를 직접 업데이트합니다.
     * 패딩/스케일링 변환은 스타일 정보에 포함된 플래그와 값들을 기반으로 수행됩니다.
     *
     * @param[in,out] object 좌표를 변환할 InferObject (내부 bbox, xy_ref_type, coord_format 멤버가 직접 수정됨).
     * @param src_style      현재 좌표의 스타일 정보 (차원 및 패딩/스케일 정보).
     * @param dst_style      변환하고자 하는 목표 스타일 정보 (차원, 형식, 기준점 및 패딩/스케일 정보).
     * @return 변환 성공 시 true, 실패 시 false.
     */
    bool ConvertInferObjectCoordinate(
        MGEN::InferObject& object,
        const ImageExpressStyle& src_style,
        const ImageExpressStyle& dst_style);

    /**
     * @brief InferObject 벡터 내의 바운딩 박스 좌표를 소스 스타일에서 목적 스타일로 변환합니다.
     * 내부적으로 ConvertInferObjectCoordinate 함수를 각 객체에 대해 호출합니다.
     *
     * @param[in,out] objects 좌표를 변환할 InferObject 벡터.
     * @param src_style       현재 좌표의 스타일 정보.
     * @param dst_style       변환하고자 하는 목표 스타일 정보.
     */
    void ConvertInferObjectsCoordinates(
        std::vector<MGEN::InferObject>& objects,
        const ImageExpressStyle& src_style,
        const ImageExpressStyle& dst_style);

}; // nsp::MGEN
namespace std
{
    // 해시 기준: infer_engine_id, class_id, score
    template <>
    struct hash<MGEN::InferObject>
    {
        std::size_t operator()(const MGEN::InferObject& obj) const
        {
            std::size_t seed = 0;

            // 1. infer_engine_id 해시 값 조합
            seed ^= std::hash<MGEN::InferEngineID>()(obj.infer_engine_id) + MGEN::HASH_MAGIC_NUMB + (seed << 6) + (seed >> 2);

            // 2. class_id 해시 값 조합
            seed ^= std::hash<MGEN::InferClassID>()(obj.class_id) + MGEN::HASH_MAGIC_NUMB + (seed << 6) + (seed >> 2);

            float  score_x_100 = obj.score * 1000;
            size_t cut_int_pos = static_cast<size_t>( score_x_100 );

            // 3. score 해시 값 조합
            seed ^= std::hash<size_t>()(cut_int_pos) + MGEN::HASH_MAGIC_NUMB + (seed << 6) + (seed >> 2);

            // 나머지 멤버들(track_id, bbox, extend_data 등)은 해시 계산에 포함하지 않음
            return seed;
        }
    };
}

#endif