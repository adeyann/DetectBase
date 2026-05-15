#pragma once

#include "ServiceProfile.h"

#include <string_view>

namespace MGEN
{
    class ServiceProfileBuilder
    {
    public:
        static ServiceProfile Build( void );
    };

    class ServiceGraphValidator
    {
    public:
        static bool Validate(
            const EntireServiceBlockProfileGraph& graph,
            std::string_view tag,
            bool log_dot = false
        );
    };

} // namespace MGEN
