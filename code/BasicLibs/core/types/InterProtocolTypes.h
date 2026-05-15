#ifndef _MGEN_INTER_PROTOCOL_TYPES_H_
#define _MGEN_INTER_PROTOCOL_TYPES_H_

#include <functional>
#include <string>
#include <optional>
#include "json/json_fwd.hpp"

namespace MGEN
{
    using InterProtocolFunc         = std::function<std::optional<nlohmann::json>(const nlohmann::json&)>;
    using InterProtocolInputChecker = std::function<bool(const nlohmann::json&)>;
}
#endif