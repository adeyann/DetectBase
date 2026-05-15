#include "sio_parser.h"
#include "json/json.hpp"
#include <iostream>
#include <stack>
#include <functional>

using namespace sio;
using json = nlohmann::json;

namespace
{
    // JSON → sio::message 변환 (재귀 금지: explicit stack DFS).
    // root_json 의 type 에 따라 array_message 또는 object_message 가 root 가 된다.
    sio::message::ptr buildSioFromJson( const json& root_json )
    {
        sio::message::ptr root_msg;
        if      ( root_json.is_object() ) root_msg = object_message::create();
        else if ( root_json.is_array()  ) root_msg = array_message::create();
        else {
            // leaf 만 들어온 경우 — 단일 message 로 즉시 반환
            if ( root_json.is_boolean() ) return bool_message::create  ( root_json.get<bool>()        );
            if ( root_json.is_number()  ) return double_message::create( root_json.get<double>()      );
            if ( root_json.is_string()  ) return string_message::create( root_json.get<std::string>() );
            if ( root_json.is_null()    ) return null_message::create();
            return null_message::create();
        }

        // 작업 단위: parent 에 'value_msg' 를 어떻게 꽂을지를 lambda assign 으로 표현
        struct WorkItem {
            std::function<void( sio::message::ptr )> assign;
            const json*                              node;
        };
        std::stack<WorkItem> stk;

        // root 의 자식을 stack 에 등록
        if ( root_json.is_object() ) {
            auto root_obj = std::dynamic_pointer_cast<object_message>( root_msg );
            for ( auto it = root_json.begin(); it != root_json.end(); ++it ) {
                std::string key = it.key();
                stk.push( WorkItem{
                    [root_obj, key]( sio::message::ptr v ) { root_obj->get_map()[key] = v; },
                    &it.value()
                } );
            }
        }
        else {
            auto root_arr = std::dynamic_pointer_cast<array_message>( root_msg );
            for ( const auto& child : root_json ) {
                stk.push( WorkItem{
                    [root_arr]( sio::message::ptr v ) { root_arr->get_vector().push_back( v ); },
                    &child
                } );
            }
        }

        while ( !stk.empty() ) {
            auto item = std::move( stk.top() );
            stk.pop();

            const json& v = *item.node;
            sio::message::ptr value_msg;

            if      ( v.is_boolean() ) value_msg = bool_message::create  ( v.get<bool>()        );
            else if ( v.is_number()  ) value_msg = double_message::create( v.get<double>()      );
            else if ( v.is_string()  ) value_msg = string_message::create( v.get<std::string>() );
            else if ( v.is_null()    ) value_msg = null_message::create  ();
            else if ( v.is_object()  ) {
                auto obj = object_message::create();
                value_msg = obj;
                for ( auto it = v.begin(); it != v.end(); ++it ) {
                    std::string key = it.key();
                    stk.push( WorkItem{
                        [obj, key]( sio::message::ptr c ) { obj->get_map()[key] = c; },
                        &it.value()
                    } );
                }
            }
            else if ( v.is_array() ) {
                auto arr = array_message::create();
                value_msg = arr;
                for ( const auto& child : v ) {
                    stk.push( WorkItem{
                        [arr]( sio::message::ptr c ) { arr->get_vector().push_back( c ); },
                        &child
                    } );
                }
            }
            else {
                std::cerr << "[buildSioFromJson] Unsupported JSON type\n";
                continue;
            }

            item.assign( value_msg );
        }

        return root_msg;
    }
}

sio::message::ptr createObject(const json& o)
{
    if (!o.is_object()) {
        std::cerr << "[createObject] called with non-object type\n";
        return object_message::create();
    }
    return buildSioFromJson( o );
}

sio::message::ptr createArray(const json& a)
{
    if (!a.is_array()) {
        std::cerr << "[createArray] called with non-array type\n";
        return array_message::create();
    }
    return buildSioFromJson( a );
}

json createJson(sio::message::ptr sio)
{
    if (!sio) {
        std::cerr << "[createJson] null pointer received\n";
        return nullptr;
    }

    try {
        switch (sio->get_flag())
        {
        case message::flag_array: {
            json j = json::array();
            for (const auto& item : sio->get_vector()) {
                j.push_back(createJson(item));
            }
            return j;
        }

        case message::flag_object: {
            json j = json::object();
            for (const auto& [key, val] : sio->get_map()) {
                j[key] = createJson(val);
            }
            return j;
        }

        case message::flag_integer:
            return sio->get_int();

        case message::flag_double:
            return sio->get_double();

        case message::flag_boolean:
            return sio->get_bool();

        case message::flag_string: {
            const std::string& str = sio->get_string();
            // nothrow 모드 파싱: 실패 시 discarded JSON 반환
            json parsed = json::parse( str, nullptr, /*allow_exceptions=*/false );
            if( parsed.is_discarded() ){
                return str; // fallback to plain string
            }
            return parsed;
        }

        case message::flag_null:
            return nullptr;

        default:
            std::cerr << "[createJson] Unknown message flag: " << sio->get_flag() << "\n";
            return nullptr;
        }
    }
    catch (const json::exception& e) {
        std::cerr << "[createJson] JSON exception: " << e.what() << "\n";
        return nullptr;
    }
}
