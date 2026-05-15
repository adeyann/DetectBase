#include "ISettingManager.h"
#include "json/json.hpp" // Should this be included? Only if used directly here.

namespace MGEN
{
    ISettingManager::ISettingManager( std::string_view unit_key_name )
        : unit_key_name_( std::string { unit_key_name } )
    {
        //
    }

    std::string ISettingManager::GetUnitKeyName( void ) const noexcept
    {
        return this->unit_key_name_;
    }
}