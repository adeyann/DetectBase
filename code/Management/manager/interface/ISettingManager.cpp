#include "ISettingManager.h"
#include "json/json.hpp" // Should this be included? Only if used directly here.

namespace MGEN
{
    // std::string_view (16B) 는 by-value pass 가 권장.
    // cppcheck-suppress passedByValue
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