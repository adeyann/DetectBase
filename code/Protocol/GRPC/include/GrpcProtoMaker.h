#pragma once

#include "MgenProto.grpc.pb.h"
#include "json/json_fwd.hpp"

#include <string>
#include <vector>

namespace MGEN
{
    EventDataOnlyJson      MakeEventOnlyJsonProto( const nlohmann::json& json_data );
    EventDataOnlyJson      MakeEventOnlyJsonProto( const std::string& json_data_string_fmt );
    EventDataOnlyJson      MakeEventOnlyJsonProto( const std::string& uuid, const nlohmann::json& json_data );
    EventDataOnlyJson      MakeEventOnlyJsonProto( const std::string& uuid, const std::string& json_data_string_fmt );

    EventDataWithRawImages MakeEventDataWithRawImagesProto( const nlohmann::json& json_data, std::vector<RawImageData>&& images );
    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& json_data_string_fmt, std::vector<RawImageData>&& images );
    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& uuid, const nlohmann::json& json_data, std::vector<RawImageData>&& images );
    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& uuid, const std::string& json_data_string_fmt, std::vector<RawImageData>&& images );

    RawImageData           MakeRawImageDataProto( std::vector<unsigned char>&& encoded_img_buffer, const int image_w, const int image_h );
}