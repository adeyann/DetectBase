#pragma once

#include "MgenTypes.h"
#include "json/json_fwd.hpp"

#include <string>
#include <string_view>
#include <set>

namespace MGEN
{
    using MGEN::Type::UnitID;

    // Base interface remains focused on core setting management actions
    class ISettingManager
    {
    public:
        // must create with 'unit_key_name'
        ISettingManager() = delete;
        // constructor with 'unit_key_name'
        ISettingManager( std::string_view unit_key_name );

        virtual ~ISettingManager() = default;

        // Getter
        std::string GetUnitKeyName( void ) const noexcept;
        virtual std::set<UnitID> GetRegistedUnitList( void ) const = 0;

        // Setter( update & remove )
        // Update or insert target unit
        virtual bool UpdateTargetUnit( const UnitID id, const nlohmann::json& update_json ) = 0;
        // Remove target unit
        virtual bool RemoveTargetUnit( const UnitID id ) = 0;

        // Bulk operations
        virtual bool UpdateBulkUnits( const nlohmann::json& update_json ) = 0;
        // Atomically replace all settings
        virtual bool RenewAfterReset( const nlohmann::json& renew_json  ) = 0;

        // NOTE: Callback registration/unregistration methods are NOT added here.
        // They are specific to the implementation (SettingManagerBase / SetterBase)
        // and would require templating or type erasure (e.g., std::any) at this
        // interface level, adding complexity. Keep the interface minimal.

    protected:
        std::string unit_key_name_ = "";
    };
}