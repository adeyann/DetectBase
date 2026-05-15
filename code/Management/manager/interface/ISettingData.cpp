#include "ISettingData.h"
#include "json/json.hpp"
#include "MgenLogger.h"

namespace MGEN
{
    ISettingData::ISettingData( const UpdateMode mode )
        : is_empty_ ( true )
        , mode_     ( mode )
        , updater_  ( nullptr )
    {
        //
    }

    void ISettingData::InitConstWithoutJSON( void ) noexcept
    {
        //
    }

    bool ISettingData::IsEmpty( void ) const noexcept
    {
        return this->is_empty_;
    }

    void ISettingData::SetEmptyFlag( const bool flag ) noexcept
    {
        this->is_empty_ = flag;
    }

    bool ISettingData::UpdateFromJson( const nlohmann::json& update_json )
    {
        if( this->updater_ == nullptr )
            return false;

        bool success = false;
        if( this->mode_ == UpdateMode::FirstOnly && update_json.is_array() && !update_json.empty() ) {
            success = this->updater_( this, update_json.front() );
        } else {
            success = this->updater_( this, update_json );
        }

        if( success ) {
            this->SetEmptyFlag( false );
            return true;
        } else {
            // 200줄 JSON dump 대신 input 요약만. 상세 실패는 내부 updater 의 MLOG_WARN 으로 식별.
            const size_t input_count = update_json.is_array() ? update_json.size() : 1;
            MLOG_ERROR( "SettingData::UpdateFromJson() Failed (input items = %zu)", input_count );
            return false;
        }
    }

    void ISettingData::SetUpdater( const SettingDataUpdater& func ) noexcept
    {
        this->updater_ = func;
    }
}