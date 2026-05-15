#pragma once

#include "json/json_fwd.hpp"
#include <functional>

namespace MGEN
{
    class ISettingData;
    using SettingDataUpdater = std::function<bool(ISettingData*, const nlohmann::json&)>;

    class ISettingData
    {
    // enum class
    public:
        enum class UpdateMode
        {
          FullArray, // JSON 배열 전체를 사용함
          FirstOnly, // 첫 번째 객체만 사용함
        };

    // methods
    public:
        // constructor
        explicit ISettingData( const UpdateMode mode = UpdateMode::FirstOnly );

        // destructor
        virtual ~ISettingData() = default;

        // 별도의 initialize json 없이도 기본값에 의해 SettingData가 생성되는 경우
        virtual void InitConstWithoutJSON( void ) noexcept;

        bool IsEmpty( void ) const noexcept;
        void SetEmptyFlag( const bool flag ) noexcept;
        bool UpdateFromJson( const nlohmann::json& update_json );

    protected:
        void SetUpdater( const SettingDataUpdater& func ) noexcept;

    private:
        bool               is_empty_ = true;
        UpdateMode         mode_     = UpdateMode::FirstOnly;
        SettingDataUpdater updater_  = nullptr;
    };
}