#include "GrpcProtoMaker.h"
#include "UUIDGenerator.h"
#include "json/json.hpp"

namespace MGEN
{
    EventDataOnlyJson MakeEventOnlyJsonProto( const nlohmann::json& json_data )
    {
        return MakeEventOnlyJsonProto( json_data.dump() );
    }

    EventDataOnlyJson MakeEventOnlyJsonProto( const std::string& json_data_string_fmt )
    {
        EventDataOnlyJson proto;

        proto.set_uuid( UUID::GetGenerator()->generate() );
        proto.set_json_data( json_data_string_fmt );

        return proto;
    }

    EventDataOnlyJson MakeEventOnlyJsonProto( const std::string& uuid, const nlohmann::json& json_data )
    {
        return MakeEventOnlyJsonProto( uuid, json_data.dump() );
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    EventDataOnlyJson MakeEventOnlyJsonProto( const std::string& uuid, const std::string& json_data_string_fmt )
    {
        EventDataOnlyJson proto;

        proto.set_uuid( uuid );
        proto.set_json_data( json_data_string_fmt );

        return proto;
    }

    EventDataWithRawImages MakeEventDataWithRawImagesProto( const nlohmann::json& json_data, std::vector<RawImageData>&& images )
    {
        return MakeEventDataWithRawImagesProto( json_data.dump(), std::move( images ) );
    }

    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& json_data_string_fmt, std::vector<RawImageData>&& images )
    {
        EventDataWithRawImages proto;

        proto.set_uuid( UUID::GetGenerator()->generate() );
        proto.set_json_data( json_data_string_fmt );

        for( const auto& image_proto : images ) {
            auto* p = proto.add_images();
            p->set_data  ( image_proto.data()   );
            p->set_size  ( image_proto.size()   );
            p->set_width ( image_proto.width()  );
            p->set_height( image_proto.height() );
        }

        return proto;
    }

    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& uuid, const nlohmann::json& json_data, std::vector<RawImageData>&& images )
    {
        return MakeEventDataWithRawImagesProto( uuid, json_data.dump(), std::move( images ) );
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    EventDataWithRawImages MakeEventDataWithRawImagesProto( const std::string& uuid, const std::string& json_data_string_fmt, std::vector<RawImageData>&& images )
    {
        EventDataWithRawImages proto;

        proto.set_uuid( uuid );
        proto.set_json_data( json_data_string_fmt );

        for( const auto& image_proto : images ) {
            auto* p = proto.add_images();
            p->set_data  ( image_proto.data()   );
            p->set_size  ( image_proto.size()   );
            p->set_width ( image_proto.width()  );
            p->set_height( image_proto.height() );
        }

        return proto;
    }

    RawImageData MakeRawImageDataProto( std::vector<unsigned char>&& encoded_img_buffer, const int image_w, const int image_h )
    {
        RawImageData proto;

        std::string encoded_image_data( encoded_img_buffer.begin(), encoded_img_buffer.end() );

        proto.set_data  ( std::move( encoded_image_data ) );
        proto.set_size  ( static_cast<int32_t>( encoded_img_buffer.size() ) );
        proto.set_width ( image_w );
        proto.set_height( image_h );

        return proto;
    }
}