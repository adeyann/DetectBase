#pragma once

#include "json/json.hpp"

/** Json Filtering Function by each Object["{targetKey}"] == value
 * @param targetJsonArray : filtering target json, type = json::Array
 * @param targetKey       : filtering target key
 * @param value           : filtering target value
 * @return : if inJson != json::Array, return empty array
 */
template <typename T>
static nlohmann::json filterJsonArray( const nlohmann::json& targetJsonArray, const std::string& targetKey, const T value )
{
    nlohmann::json result = nlohmann::json::array();
    if( targetJsonArray.is_array() == false )
        return result;

    for( const auto& obj : targetJsonArray ) {
        if( obj.is_object() && obj.contains( targetKey ) && obj[targetKey].get<T>() == value )
            result.push_back( obj );
    }
    return result;
}
