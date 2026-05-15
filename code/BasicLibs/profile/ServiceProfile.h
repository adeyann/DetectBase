#ifndef _MGEN_SERVICE_PROFILE_H_
#define _MGEN_SERVICE_PROFILE_H_

#include "ServiceStreamTypes.h"
#include "ServiceBlockProfile.h"

#include <string>
#include <string_view>
#include <optional>

namespace MGEN
{
    namespace EntireServiceTag
    {
        constexpr std::string_view DETECTOR  = "DETECTOR";
    }

    class ServiceProfile
    {
    public:
        // Default constructor delete
        ServiceProfile() = delete;

        // Base constructor
        explicit ServiceProfile( std::string_view service_name, const EntireServiceBlockProfileGraph& service_block_graph ) noexcept
            : service_name_ ( std::string { service_name } )
            , service_graph_( service_block_graph )
        {
        }

        // Destructor
        ~ServiceProfile() = default;

        // Getter : entire service name
        std::string GetServiceName() const noexcept
        {
            return service_name_;
        }

        // Getter : servie block
        const EntireServiceBlockProfileGraph& GetServiceGraph() const noexcept
        {
            return service_graph_;
        }

        std::optional<ServiceBlockProfile> GetBlockProfile( ServiceBlockModuleType type ) const
        {
            return service_graph_.GetBlockProfile( type );
        }

    private:
        // information for entire service
        const std::string                    service_name_;
        const EntireServiceBlockProfileGraph service_graph_;
    };

} // namespace MGEN

#endif // _MGEN_SERVICE_PROFILE_H_
